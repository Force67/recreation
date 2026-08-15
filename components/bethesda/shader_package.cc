#include "components/bethesda/shader_package.h"

#include <base/memory/move.h>

#include <cstring>

namespace rx::bethesda {
namespace {

// Every .fxp shader record opens with this.
constexpr u32 kRecordMagic = 0x11223344;
// The descriptor size is fixed per slot, which is what makes the walk
// deterministic: the slot a record sits in says how many bytes precede its
// bytecode. The two shipped layouts disagree on the sizes as well as on how
// many slots a group header lists.
constexpr size_t kSkyrimVertexDescriptor = 32;
constexpr size_t kSkyrimPixelDescriptor = 68;
constexpr size_t kSkyrimComputeDescriptor = 36;
constexpr size_t kFallout4PixelDescriptor = 36;
constexpr size_t kFallout4OtherDescriptor = 44;

// Where the first record magic lands tells the layouts apart: Skyrim SE opens
// with two counts, Fallout 4 with five.
constexpr size_t kSkyrimFirstRecord = 8;
constexpr size_t kFallout4FirstRecord = 20;

enum class FxpLayout { kSkyrim, kFallout4 };

constexpr u32 kMaxSlots = 5;

constexpr u32 kSdpVersion = 100;
// Fixed-width name field, NUL-terminated and padded with filler.
constexpr size_t kSdpNameField = 256;

struct Reader {
  const u8* p;
  const u8* end;
  bool ok = true;

  size_t Left() const { return static_cast<size_t>(end - p); }
  bool Need(size_t n) {
    if (!ok || Left() < n)
      ok = false;
    return ok;
  }
  u32 U32() {
    if (!Need(4))
      return 0;
    u32 v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }
  u64 U64() {
    if (!Need(8))
      return 0;
    u64 v;
    std::memcpy(&v, p, 8);
    p += 8;
    return v;
  }
  const u8* Bytes(size_t n) {
    if (!Need(n))
      return nullptr;
    const u8* at = p;
    p += n;
    return at;
  }
};

u32 ReadU32At(const u8* p) {
  u32 v;
  std::memcpy(&v, p, 4);
  return v;
}

bool IsDxbc(const u8* p) {
  return p[0] == 'D' && p[1] == 'X' && p[2] == 'B' && p[3] == 'C';
}

// DXBC header: magic, 16-byte digest, u32 version, u32 total size, u32 chunk
// count, then the chunk offset table.
constexpr size_t kDxbcTotalSizeOffset = 0x18;
constexpr size_t kDxbcChunkCountOffset = 0x1c;
constexpr size_t kDxbcChunkTableOffset = 0x20;

u32 DxbcTotalSize(const u8* p, size_t avail) {
  return avail >= kDxbcTotalSizeOffset + 4 ? ReadU32At(p + kDxbcTotalSizeOffset) : 0;
}

// The stage lives in the high half of the shader chunk's version token. Both
// the SM4 (SHDR) and SM5 (SHEX) spellings appear across the games.
ShaderStage DxbcStage(const u8* p, size_t avail) {
  if (avail < kDxbcChunkTableOffset + 4)
    return ShaderStage::kUnknown;
  const u32 chunks = ReadU32At(p + kDxbcChunkCountOffset);
  if (avail < kDxbcChunkTableOffset + static_cast<size_t>(chunks) * 4)
    return ShaderStage::kUnknown;
  for (u32 i = 0; i < chunks; ++i) {
    const u32 offset = ReadU32At(p + kDxbcChunkTableOffset + i * 4);
    if (avail < static_cast<size_t>(offset) + 12)
      continue;
    const u8* chunk = p + offset;
    const bool shader = (chunk[0] == 'S' && chunk[1] == 'H' &&
                         ((chunk[2] == 'E' && chunk[3] == 'X') || (chunk[2] == 'D' && chunk[3] == 'R')));
    if (!shader)
      continue;
    switch (ReadU32At(chunk + 8) >> 16) {
      case 0:
        return ShaderStage::kPixel;
      case 1:
        return ShaderStage::kVertex;
      case 2:
        return ShaderStage::kGeometry;
      case 3:
        return ShaderStage::kHull;
      case 4:
        return ShaderStage::kDomain;
      case 5:
        return ShaderStage::kCompute;
      default:
        return ShaderStage::kUnknown;
    }
  }
  return ShaderStage::kUnknown;
}

// D3D9 bytecode opens with a version token whose high half says which stage:
// 0xfffe is a vertex shader, 0xffff a pixel shader.
ShaderStage D3d9Stage(const u8* p, size_t avail) {
  if (avail < 4)
    return ShaderStage::kUnknown;
  switch (ReadU32At(p) >> 16) {
    case 0xfffe:
      return ShaderStage::kVertex;
    case 0xffff:
      return ShaderStage::kPixel;
    default:
      return ShaderStage::kUnknown;
  }
}

// Reads one .fxp record: magic, bytecode size, technique id, the fixed-size
// descriptor for its slot, then the container itself.
bool ReadFxpRecord(Reader& r, u32 group, size_t descriptor_len, PackagedShader* out) {
  if (r.U32() != kRecordMagic || !r.ok)
    return false;
  const u32 size = r.U32();
  out->technique_id = r.U32();
  if (!r.ok || size < kDxbcChunkTableOffset)
    return false;

  const u8* descriptor = r.Bytes(descriptor_len);
  const u8* body = r.Bytes(size);
  if (!r.ok || !descriptor || !body)
    return false;
  if (!IsDxbc(body) || DxbcTotalSize(body, size) != size)
    return false;

  out->group = group;
  out->stage = DxbcStage(body, size);
  out->descriptor = ByteSpan(descriptor, descriptor_len);
  out->bytecode = ByteSpan(body, size);
  // Vertex descriptors lead with the packed vertex layout.
  if (out->stage == ShaderStage::kVertex && descriptor_len >= 8)
    std::memcpy(&out->vertex_desc, descriptor, 8);
  return true;
}

// The slots a group header lists, in the order their records follow.
struct SlotLayout {
  u32 count = 0;
  size_t descriptor = 0;
  u32 ShaderGroup::*field = nullptr;
};

// Fills in one group's slots, consuming its header. Returns how many slots the
// header described.
u32 ReadGroupHeader(Reader& r, FxpLayout layout, SlotLayout* slots, ShaderGroup* group) {
  if (layout == FxpLayout::kFallout4) {
    const SlotLayout kSlots[kMaxSlots] = {
        {0, kFallout4OtherDescriptor, &ShaderGroup::vertex_count},
        {0, kFallout4OtherDescriptor, &ShaderGroup::hull_count},
        {0, kFallout4OtherDescriptor, &ShaderGroup::domain_count},
        {0, kFallout4PixelDescriptor, &ShaderGroup::pixel_count},
        {0, kFallout4OtherDescriptor, &ShaderGroup::compute_count},
    };
    for (u32 i = 0; i < kMaxSlots; ++i) {
      slots[i] = kSlots[i];
      slots[i].count = r.U32();
      group->*(slots[i].field) = slots[i].count;
    }
    return kMaxSlots;
  }

  // Skyrim SE: a vertex and a pixel count, except compute groups, which carry
  // a single count and are told apart because the record magic sits where a
  // second count otherwise would.
  const u32 first = r.U32();
  if (!r.ok)
    return 0;
  if (ReadU32At(r.p) == kRecordMagic) {
    slots[0] = {first, kSkyrimComputeDescriptor, &ShaderGroup::compute_count};
    group->compute_count = first;
    return 1;
  }
  slots[0] = {first, kSkyrimVertexDescriptor, &ShaderGroup::vertex_count};
  slots[1] = {r.U32(), kSkyrimPixelDescriptor, &ShaderGroup::pixel_count};
  group->vertex_count = slots[0].count;
  group->pixel_count = slots[1].count;
  return 2;
}

ShaderPackage ParseFxp(ByteSpan data, FxpLayout layout) {
  ShaderPackage package;
  Reader r{data.data(), data.data() + data.size()};

  while (r.ok && r.Left() >= 8) {
    ShaderGroup group;
    group.first_shader = static_cast<u32>(package.shaders.size());

    SlotLayout slots[kMaxSlots];
    const u32 slot_count = ReadGroupHeader(r, layout, slots, &group);
    if (!r.ok || slot_count == 0)
      return package;

    u64 total = 0;
    for (u32 s = 0; s < slot_count; ++s)
      total += slots[s].count;
    // A group header claiming more shaders than the blob could hold means the
    // walk has lost sync; stop rather than allocate on garbage.
    if (total * 12 > r.Left())
      return package;

    const u32 group_index = static_cast<u32>(package.groups.size());
    for (u32 s = 0; s < slot_count; ++s) {
      for (u32 i = 0; i < slots[s].count; ++i) {
        PackagedShader shader;
        if (!ReadFxpRecord(r, group_index, slots[s].descriptor, &shader))
          return package;
        package.shaders.push_back(base::move(shader));
      }
    }
    group.shader_count = static_cast<u32>(total);
    package.groups.push_back(group);
  }

  package.valid = r.ok && r.Left() == 0;
  return package;
}

ShaderPackage ParseSdp(ByteSpan data) {
  ShaderPackage package;
  Reader r{data.data(), data.data() + data.size()};

  r.U32();  // version, already checked by the dispatcher
  const u32 count = r.U32();
  r.U32();  // total size of the record area
  if (!r.ok)
    return package;

  for (u32 i = 0; i < count; ++i) {
    const u8* name = r.Bytes(kSdpNameField);
    const u32 size = r.U32();
    if (!r.ok || !name)
      return package;
    const u8* body = r.Bytes(size);
    if (!r.ok || !body)
      return package;

    PackagedShader shader;
    size_t name_len = 0;
    while (name_len < kSdpNameField && name[name_len] != 0)
      ++name_len;
    shader.name = base::String(reinterpret_cast<const char*>(name), name_len);
    shader.stage = D3d9Stage(body, size);
    shader.bytecode = ByteSpan(body, size);
    package.shaders.push_back(base::move(shader));
  }

  package.valid = r.ok;
  return package;
}

}  // namespace

const char* ShaderStageName(ShaderStage stage) {
  switch (stage) {
    case ShaderStage::kVertex:
      return "vs";
    case ShaderStage::kPixel:
      return "ps";
    case ShaderStage::kGeometry:
      return "gs";
    case ShaderStage::kHull:
      return "hs";
    case ShaderStage::kDomain:
      return "ds";
    case ShaderStage::kCompute:
      return "cs";
    case ShaderStage::kUnknown:
      break;
  }
  return "unknown";
}

ShaderPackage ParseShaderPackage(ByteSpan data) {
  if (data.size() >= kSkyrimFirstRecord + 4 &&
      ReadU32At(data.data() + kSkyrimFirstRecord) == kRecordMagic)
    return ParseFxp(data, FxpLayout::kSkyrim);
  if (data.size() >= kFallout4FirstRecord + 4 &&
      ReadU32At(data.data() + kFallout4FirstRecord) == kRecordMagic)
    return ParseFxp(data, FxpLayout::kFallout4);
  if (data.size() >= 12 && ReadU32At(data.data()) == kSdpVersion)
    return ParseSdp(data);
  return ShaderPackage{};
}

}  // namespace rx::bethesda

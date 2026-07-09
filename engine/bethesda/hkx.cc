#include "bethesda/hkx.h"

#include <cstring>

namespace rx::bethesda {
namespace {

constexpr u32 kMagic0 = 0x57E0E057u;
constexpr u32 kMagic1 = 0x10C0C010u;

struct SectionHeader {
  char tag[20];  // 19 chars + 0xFF separator
  u32 absolute_data_start;
  u32 local_fixups_offset;
  u32 global_fixups_offset;
  u32 virtual_fixups_offset;
  u32 exports_offset;
  u32 imports_offset;
  u32 end_offset;
};
static_assert(sizeof(SectionHeader) == 48);

u32 ReadU32(const u8* p) {
  u32 v;
  std::memcpy(&v, p, 4);
  return v;
}

}  // namespace

std::optional<HkxFile> HkxFile::Parse(const u8* bytes, size_t size) {
  if (size < 64) return std::nullopt;
  if (ReadU32(bytes) != kMagic0 || ReadU32(bytes + 4) != kMagic1) return std::nullopt;

  HkxFile file;
  const u32 file_version = ReadU32(bytes + 12);
  file.pointer_size_ = bytes[16];
  const u8 little_endian = bytes[17];
  const u32 num_sections = ReadU32(bytes + 20);
  // contentsSectionIndex (24) names the data section; Skyrim files always use
  // the {classnames, types, data} triple, so we identify by tag instead.
  file.content_version_.assign(reinterpret_cast<const char*>(bytes + 40),
                               strnlen(reinterpret_cast<const char*>(bytes + 40), 16));
  if (!little_endian) return std::nullopt;  // big-endian consoles: not ours
  // v8/v9 are the hk2010-2013 era (Skyrim LE/SE and most mod toolchains);
  // v11 is hk2014 (Fallout 4/76). v11 pads every section header to 64 bytes.
  if (file_version != 8 && file_version != 9 && file_version != 11) return std::nullopt;
  const size_t section_stride = file_version >= 11 ? 64 : sizeof(SectionHeader);
  if (file.pointer_size_ != 4 && file.pointer_size_ != 8) return std::nullopt;
  if (num_sections == 0 || num_sections > 8) return std::nullopt;
  if (64 + static_cast<size_t>(num_sections) * section_stride > size) return std::nullopt;

  const SectionHeader* classnames = nullptr;
  const SectionHeader* data = nullptr;
  std::vector<const SectionHeader*> sections(num_sections);
  for (u32 i = 0; i < num_sections; ++i) {
    const auto* section =
        reinterpret_cast<const SectionHeader*>(bytes + 64 + i * section_stride);
    sections[i] = section;
    std::string_view tag(section->tag, strnlen(section->tag, 19));
    if (tag == "__classnames__") classnames = section;
    if (tag == "__data__") data = section;
  }
  if (!classnames || !data) return std::nullopt;

  auto section_valid = [&](const SectionHeader* s) {
    return static_cast<size_t>(s->absolute_data_start) + s->end_offset <= size &&
           s->local_fixups_offset <= s->end_offset &&
           s->global_fixups_offset <= s->end_offset &&
           s->virtual_fixups_offset <= s->end_offset;
  };
  if (!section_valid(classnames) || !section_valid(data)) return std::nullopt;

  file.classnames_.assign(reinterpret_cast<const char*>(bytes + classnames->absolute_data_start),
                          classnames->local_fixups_offset);  // names end where fixups begin
  file.data_.assign(bytes + data->absolute_data_start,
                    bytes + data->absolute_data_start + data->local_fixups_offset);

  const u8* section_base = bytes + data->absolute_data_start;

  // Local fixups: { u32 from, u32 to }, 0xFFFFFFFF entries are padding.
  for (u32 off = data->local_fixups_offset; off + 8 <= data->global_fixups_offset; off += 8) {
    u32 from = ReadU32(section_base + off);
    u32 to = ReadU32(section_base + off + 4);
    if (from == 0xFFFFFFFFu) continue;
    file.pointers_[from] = to;
  }
  // Global fixups: { u32 from, u32 dstSection, u32 dstOffset }. Skyrim's
  // object graph lives entirely in __data__, so a reference into any other
  // section would be a file this reader doesn't model; skip those entries.
  const u32 data_section_index = [&] {
    for (u32 i = 0; i < num_sections; ++i) {
      if (sections[i] == data) return i;
    }
    return 0u;
  }();
  for (u32 off = data->global_fixups_offset; off + 12 <= data->virtual_fixups_offset; off += 12) {
    u32 from = ReadU32(section_base + off);
    u32 dst_section = ReadU32(section_base + off + 4);
    u32 to = ReadU32(section_base + off + 8);
    if (from == 0xFFFFFFFFu) continue;
    if (dst_section != data_section_index) continue;
    file.pointers_[from] = to;
  }
  // Virtual fixups: { u32 objOffset, u32 classnameSection, u32 nameOffset }.
  for (u32 off = data->virtual_fixups_offset; off + 12 <= data->exports_offset; off += 12) {
    u32 obj = ReadU32(section_base + off);
    u32 name_off = ReadU32(section_base + off + 8);
    if (obj == 0xFFFFFFFFu) continue;
    if (name_off >= file.classnames_.size()) continue;
    const char* name = file.classnames_.c_str() + name_off;
    file.object_at_[obj] = static_cast<u32>(file.objects_.size());
    file.objects_.push_back({obj, std::string_view(name)});
  }
  return file;
}

std::string_view HkxFile::class_of(u64 offset) const {
  auto it = object_at_.find(offset);
  return it == object_at_.end() ? std::string_view{} : objects_[it->second].class_name;
}

u8 HkxFile::U8(u64 offset) const {
  return offset + 1 <= data_.size() ? data_[offset] : 0;
}
u16 HkxFile::U16(u64 offset) const {
  if (offset + 2 > data_.size()) return 0;
  u16 v;
  std::memcpy(&v, data_.data() + offset, 2);
  return v;
}
u32 HkxFile::U32(u64 offset) const {
  if (offset + 4 > data_.size()) return 0;
  u32 v;
  std::memcpy(&v, data_.data() + offset, 4);
  return v;
}
u64 HkxFile::U64(u64 offset) const {
  if (offset + 8 > data_.size()) return 0;
  u64 v;
  std::memcpy(&v, data_.data() + offset, 8);
  return v;
}
i16 HkxFile::I16(u64 offset) const {
  if (offset + 2 > data_.size()) return 0;
  i16 v;
  std::memcpy(&v, data_.data() + offset, 2);
  return v;
}
f32 HkxFile::F32(u64 offset) const {
  if (offset + 4 > data_.size()) return 0.0f;
  f32 v;
  std::memcpy(&v, data_.data() + offset, 4);
  return v;
}

u64 HkxFile::Pointer(u64 offset) const {
  auto it = pointers_.find(offset);
  return it == pointers_.end() ? kNull : it->second;
}

std::string_view HkxFile::CString(u64 offset) const {
  u64 target = Pointer(offset);
  if (target == kNull || target >= data_.size()) return {};
  const char* start = reinterpret_cast<const char*>(data_.data() + target);
  size_t max = data_.size() - target;
  return std::string_view(start, strnlen(start, max));
}

u64 HkxFile::Array(u64 offset, u32* count) const {
  *count = 0;
  u64 elems = Pointer(offset);
  if (elems == kNull) return kNull;
  u32 size = U32(offset + pointer_size_);
  // capacityAndFlags rides in the top bits of the next int; size itself is
  // a plain count.
  *count = size;
  return elems;
}

}  // namespace rx::bethesda

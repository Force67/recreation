#include <cstring>
#include <fstream>
#include <unordered_map>

#include "asset/asset_id.h"
#include "bethesda/archive.h"
#include "bethesda/compression.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr u32 kBa2Magic = FourCc('B', 'T', 'D', 'X');
constexpr u32 kBa2Gnrl = FourCc('G', 'N', 'R', 'L');
constexpr u32 kBa2Dx10 = FourCc('D', 'X', '1', '0');

struct Ba2Header {
  u32 magic;
  u32 version;  // 1/7/8 FO4, 2/3 Starfield
  u32 type;     // GNRL or DX10
  u32 file_count;
  u64 name_table_offset;
};
static_assert(sizeof(Ba2Header) == 24);

// Starfield inserts an extra header between the 24 byte base header and the file
// records; the per-file record and texture chunk layouts are otherwise
// identical to FO4. v2 adds a u64; v3 (texture archives) adds that u64 plus a
// u32 (value 3), so its records start 12 bytes in. Getting this wrong reads the
// texture dimensions from the wrong offset and tries to allocate a garbage size.
constexpr size_t ExtraHeaderSize(u32 version) {
  if (version == 2) return sizeof(u64);
  if (version == 3) return sizeof(u64) + sizeof(u32);
  return 0;
}

// GNRL file record, 36 bytes on disk. A packed size of 0 means the bytes are
// stored uncompressed; otherwise they are a zlib (DEFLATE) stream.
struct GnrlEntry {
  u64 offset = 0;
  u32 packed_size = 0;
  u32 full_size = 0;
};

// DX10 texture: a tex header followed by per mip-range chunks. The chunks
// reassemble into the mip chain of a DDS, whose header we synthesize on read.
struct TexChunk {
  u64 offset = 0;
  u32 packed_size = 0;
  u32 full_size = 0;
};
struct TexEntry {
  u16 width = 0;
  u16 height = 0;
  u8 mip_count = 0;
  u8 dxgi_format = 0;
  base::Vector<TexChunk> chunks;
};

// Reads `entry`'s bytes from `file`, inflating when packed. Returns false on a
// short read or a decompression failure.
bool ReadBlock(std::ifstream& file, u64 offset, u32 packed_size, u32 full_size,
               u8* dst) {
  file.seekg(static_cast<std::streamoff>(offset));
  if (packed_size == 0) {
    file.read(reinterpret_cast<char*>(dst), full_size);
    return static_cast<bool>(file);
  }
  base::Vector<u8> packed(packed_size);
  file.read(reinterpret_cast<char*>(packed.data()), packed_size);
  if (!file) return false;
  return ZlibInflate(ByteSpan(packed.data(), packed.size()), dst, full_size);
}

void PutU32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

template <typename T>
T GetLe(const u8* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

constexpr size_t kGnrlRecordSize = 36;
constexpr size_t kTexHeaderSize = 24;
constexpr size_t kTexChunkSize = 24;

// Builds a DDS file (4 byte magic + 124 byte header + 20 byte DX10 header)
// describing `tex`, leaving room for the mip data appended by the caller. The
// field offsets line up with what ConvertDds reads (height@12, width@16,
// mips@28, pf flags@80, fourcc@84, caps2@112, dxgi@128, data@148).
base::Vector<u8> MakeDdsHeader(const TexEntry& tex) {
  constexpr u32 kDdsHeaderSize = 124;
  constexpr u32 kDdpfFourCc = 0x4;
  constexpr u32 kDdsHeaderFlags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;  // caps|h|w|pf|mips
  constexpr u32 kDdsCapsTexture = 0x1000 | 0x400000 | 0x8;             // tex|mips|complex
  constexpr u32 kD3d10Dimension2d = 3;

  base::Vector<u8> dds(148);  // value-initialized to zero
  PutU32(dds.data() + 0, FourCc('D', 'D', 'S', ' '));
  PutU32(dds.data() + 4, kDdsHeaderSize);
  PutU32(dds.data() + 8, kDdsHeaderFlags);
  PutU32(dds.data() + 12, tex.height);
  PutU32(dds.data() + 16, tex.width);
  PutU32(dds.data() + 28, tex.mip_count);
  PutU32(dds.data() + 76, 32);  // DDS_PIXELFORMAT dwSize
  PutU32(dds.data() + 80, kDdpfFourCc);
  PutU32(dds.data() + 84, FourCc('D', 'X', '1', '0'));
  PutU32(dds.data() + 108, kDdsCapsTexture);
  PutU32(dds.data() + 128, tex.dxgi_format);
  PutU32(dds.data() + 132, kD3d10Dimension2d);
  PutU32(dds.data() + 140, 1);  // arraySize
  return dds;
}

class Ba2Provider final : public asset::FileProvider {
 public:
  Ba2Provider(std::string path, Ba2Header header)
      : path_(std::move(path)), header_(header) {}

  bool Parse() {
    std::ifstream file(path_, std::ios::binary);
    if (!file) return false;
    file.seekg(sizeof(Ba2Header) + ExtraHeaderSize(header_.version));

    const bool dx10 = header_.type == kBa2Dx10;
    base::Vector<GnrlEntry> gnrl;
    base::Vector<TexEntry> tex;
    if (dx10)
      tex.reserve(header_.file_count);
    else
      gnrl.reserve(header_.file_count);

    // Records are read through fixed-size byte buffers (not packed structs) so
    // field offsets stay exact regardless of how the compiler would align a
    // struct holding a u64.
    u8 buffer[kTexHeaderSize];
    for (u32 i = 0; i < header_.file_count; ++i) {
      if (dx10) {
        file.read(reinterpret_cast<char*>(buffer), kTexHeaderSize);
        if (!file) return false;
        const u8 chunk_count = buffer[13];
        TexEntry entry;
        entry.height = GetLe<u16>(buffer + 16);
        entry.width = GetLe<u16>(buffer + 18);
        entry.mip_count = buffer[20];
        entry.dxgi_format = buffer[21];
        entry.chunks.reserve(chunk_count);
        for (u8 c = 0; c < chunk_count; ++c) {
          u8 ch[kTexChunkSize];
          file.read(reinterpret_cast<char*>(ch), kTexChunkSize);
          if (!file) return false;
          entry.chunks.push_back(
              {GetLe<u64>(ch), GetLe<u32>(ch + 8), GetLe<u32>(ch + 12)});
        }
        tex.push_back(std::move(entry));
      } else {
        u8 fr[kGnrlRecordSize];
        file.read(reinterpret_cast<char*>(fr), kGnrlRecordSize);
        if (!file) return false;
        gnrl.push_back({GetLe<u64>(fr + 16), GetLe<u32>(fr + 24), GetLe<u32>(fr + 28)});
      }
    }

    // Name table: file_count entries of u16 length + that many path bytes, in
    // the same order as the records. Paths use backslashes and mixed case;
    // NormalizePath folds them to the lower-case forward-slash lookup keys.
    file.seekg(static_cast<std::streamoff>(header_.name_table_offset));
    for (u32 i = 0; i < header_.file_count; ++i) {
      u16 length = 0;
      file.read(reinterpret_cast<char*>(&length), 2);
      if (!file) return false;
      std::string name(length, '\0');
      file.read(name.data(), length);
      if (!file) return false;
      std::string key = asset::NormalizePath(name);
      if (dx10)
        tex_entries_.emplace(std::move(key), std::move(tex[i]));
      else
        entries_.emplace(std::move(key), gnrl[i]);
    }
    return true;
  }

  bool Contains(std::string_view normalized_path) const override {
    std::string key(normalized_path);
    return entries_.contains(key) || tex_entries_.contains(key);
  }

  std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const override {
    std::string key(normalized_path);
    std::ifstream file(path_, std::ios::binary);
    if (!file) return std::nullopt;

    if (auto it = entries_.find(key); it != entries_.end()) {
      const GnrlEntry& e = it->second;
      base::Vector<u8> data(e.full_size);
      if (!ReadBlock(file, e.offset, e.packed_size, e.full_size, data.data())) {
        REC_WARN("ba2 read failed: {} in {}", normalized_path, path_);
        return std::nullopt;
      }
      return data;
    }

    if (auto it = tex_entries_.find(key); it != tex_entries_.end()) {
      const TexEntry& tex = it->second;
      base::Vector<u8> dds = MakeDdsHeader(tex);
      const size_t header_size = dds.size();
      size_t total = header_size;
      for (const TexChunk& c : tex.chunks) total += c.full_size;
      dds.resize(total);
      size_t cursor = header_size;
      for (const TexChunk& c : tex.chunks) {
        if (!ReadBlock(file, c.offset, c.packed_size, c.full_size, dds.data() + cursor)) {
          REC_WARN("ba2 tex read failed: {} in {}", normalized_path, path_);
          return std::nullopt;
        }
        cursor += c.full_size;
      }
      return dds;
    }
    return std::nullopt;
  }

  void Enumerate(const std::function<void(std::string_view)>& fn) const override {
    for (const auto& [name, entry] : entries_) fn(name);
    for (const auto& [name, entry] : tex_entries_) fn(name);
  }

  std::string name() const override { return path_; }

 private:
  std::string path_;
  Ba2Header header_;
  // std::string keyed maps stay STL, matching the Vfs path convention.
  std::unordered_map<std::string, GnrlEntry> entries_;
  std::unordered_map<std::string, TexEntry> tex_entries_;
};

bool IsKnownVersion(u32 version) {
  return version == 1 || version == 2 || version == 3 || version == 7 || version == 8;
}

}  // namespace

base::UniquePointer<asset::FileProvider> OpenBa2(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return nullptr;
  Ba2Header header{};
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!file || header.magic != kBa2Magic) {
    REC_ERROR("not a ba2: {}", path);
    return nullptr;
  }
  if (!IsKnownVersion(header.version) ||
      (header.type != kBa2Gnrl && header.type != kBa2Dx10)) {
    REC_ERROR("unsupported ba2 (v{}) in {}", header.version, path);
    return nullptr;
  }
  file.close();
  auto provider = base::MakeUnique<Ba2Provider>(path, header);
  if (!provider->Parse()) {
    REC_ERROR("failed to parse ba2 tables: {}", path);
    return nullptr;
  }
  REC_INFO("ba2 {}: v{}, {} files", path, header.version, header.file_count);
  return provider;
}

base::UniquePointer<asset::FileProvider> OpenArchive(const std::string& path) {
  if (path.ends_with(".bsa") || path.ends_with(".BSA")) return OpenBsa(path);
  if (path.ends_with(".ba2") || path.ends_with(".BA2")) return OpenBa2(path);
  return nullptr;
}

}  // namespace rec::bethesda

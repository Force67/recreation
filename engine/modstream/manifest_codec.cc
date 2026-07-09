#include "modstream/manifest_codec.h"

namespace rx::modstream {
namespace {

constexpr u32 kMagic = FourCc('R', 'M', 'D', '1');
constexpr u16 kVersion = 1;

// Sanity ceilings so a corrupt count can never make the decoder allocate wildly
// or loop forever. A real server stays orders of magnitude under these.
constexpr u32 kMaxResources = 64 * 1024;
constexpr u32 kMaxFilesPerResource = 1024 * 1024;
constexpr u16 kMaxNameLen = 4096;

// Little-endian append helpers; the reader below mirrors them.
void PutU16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v));
  out.push_back(static_cast<u8>(v >> 8));
}

void PutU32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void PutU64(std::vector<u8>& out, u64 v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void PutString(std::vector<u8>& out, const std::string& s) {
  PutU16(out, static_cast<u16>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

// Cursor over the received bytes. Every read checks `Need` first and flips `ok`
// to false on underrun, so callers can read optimistically and test ok() once.
class Reader {
 public:
  Reader(const u8* data, size_t size) : data_(data), size_(size) {}

  u16 U16() {
    if (!Need(2)) return 0;
    const u16 v = static_cast<u16>(data_[pos_]) |
                  static_cast<u16>(data_[pos_ + 1]) << 8;
    pos_ += 2;
    return v;
  }

  u32 U32() {
    if (!Need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return v;
  }

  u64 U64() {
    if (!Need(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return v;
  }

  std::string String(u16 max_len) {
    const u16 len = U16();
    if (len > max_len || !Need(len)) {
      ok_ = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return s;
  }

  bool ok() const { return ok_; }
  bool at_end() const { return pos_ == size_; }

 private:
  bool Need(size_t n) {
    if (pos_ + n > size_) {
      ok_ = false;
      return false;
    }
    return true;
  }

  const u8* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace

std::vector<u8> EncodeManifest(const ModManifest& manifest) {
  std::vector<u8> out;
  PutU32(out, kMagic);
  PutU16(out, kVersion);
  PutU32(out, static_cast<u32>(manifest.resources.size()));
  for (const ModResource& resource : manifest.resources) {
    PutString(out, resource.name);
    PutU32(out, static_cast<u32>(resource.files.size()));
    for (const ResourceFile& file : resource.files) {
      PutString(out, file.path);
      PutU64(out, file.size);
      PutU64(out, file.hash);
    }
  }
  return out;
}

std::optional<ModManifest> DecodeManifest(const u8* data, size_t size) {
  Reader r(data, size);
  if (r.U32() != kMagic) return std::nullopt;
  if (r.U16() != kVersion) return std::nullopt;

  const u32 resource_count = r.U32();
  if (!r.ok() || resource_count > kMaxResources) return std::nullopt;

  ModManifest manifest;
  manifest.resources.reserve(resource_count);
  for (u32 i = 0; i < resource_count; ++i) {
    ModResource resource;
    resource.name = r.String(kMaxNameLen);
    const u32 file_count = r.U32();
    if (!r.ok() || file_count > kMaxFilesPerResource) return std::nullopt;
    resource.files.reserve(file_count);
    for (u32 j = 0; j < file_count; ++j) {
      ResourceFile file;
      file.path = r.String(kMaxNameLen);
      file.size = r.U64();
      file.hash = r.U64();
      if (!r.ok()) return std::nullopt;
      resource.files.push_back(std::move(file));
    }
    manifest.resources.push_back(std::move(resource));
  }

  // A well-formed manifest consumes the buffer exactly; trailing bytes mean the
  // payload is not what it claims to be.
  if (!r.ok() || !r.at_end()) return std::nullopt;
  return manifest;
}

}  // namespace rx::modstream

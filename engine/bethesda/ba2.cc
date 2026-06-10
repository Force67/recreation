#include <cstring>
#include <fstream>

#include "bethesda/archive.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr u32 kBa2Magic = FourCc('B', 'T', 'D', 'X');
constexpr u32 kBa2Gnrl = FourCc('G', 'N', 'R', 'L');
constexpr u32 kBa2Dx10 = FourCc('D', 'X', '1', '0');

struct Ba2Header {
  u32 magic;
  u32 version;  // 1/7/8 FO4, 2/3 FO76
  u32 type;     // GNRL or DX10
  u32 file_count;
  u64 name_table_offset;
};

class Ba2Provider final : public asset::FileProvider {
 public:
  Ba2Provider(std::string path, Ba2Header header)
      : path_(std::move(path)), header_(header) {}

  bool Contains(std::string_view) const override { return false; }
  std::optional<std::vector<u8>> Read(std::string_view) const override { return std::nullopt; }
  void Enumerate(const std::function<void(std::string_view)>&) const override {}
  std::string name() const override { return path_; }

  // TODO: GNRL file entries (zlib chunks) and DX10 texture entries (per mip
  // chunks reassembled into a dds layout), name table at name_table_offset.

 private:
  std::string path_;
  Ba2Header header_;
};

bool IsKnownVersion(u32 version) {
  return version == 1 || version == 2 || version == 3 || version == 7 || version == 8;
}

}  // namespace

std::unique_ptr<asset::FileProvider> OpenBa2(const std::string& path) {
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
  REC_INFO("ba2 {}: v{}, {} files", path, header.version, header.file_count);
  return std::make_unique<Ba2Provider>(path, header);
}

std::unique_ptr<asset::FileProvider> OpenArchive(const std::string& path) {
  if (path.ends_with(".bsa") || path.ends_with(".BSA")) return OpenBsa(path);
  if (path.ends_with(".ba2") || path.ends_with(".BA2")) return OpenBa2(path);
  return nullptr;
}

}  // namespace rec::bethesda

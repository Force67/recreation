#include <cstring>
#include <fstream>

#include "bethesda/archive.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr u32 kBsaMagic = FourCc('B', 'S', 'A', '\0');

struct BsaHeader {
  u32 magic;
  u32 version;  // 104 LE/Skyrim, 105 SSE
  u32 folder_records_offset;
  u32 archive_flags;
  u32 folder_count;
  u32 file_count;
  u32 total_folder_name_length;
  u32 total_file_name_length;
  u32 file_flags;
};

class BsaProvider final : public asset::FileProvider {
 public:
  BsaProvider(std::string path, BsaHeader header)
      : path_(std::move(path)), header_(header) {}

  bool Contains(std::string_view) const override { return false; }
  std::optional<std::vector<u8>> Read(std::string_view) const override { return std::nullopt; }
  void Enumerate(const std::function<void(std::string_view)>&) const override {}
  std::string name() const override { return path_; }

  // TODO: folder/file record tables (hash, size, offset), name block,
  // optional per file zlib/lz4 compression depending on archive_flags.

 private:
  std::string path_;
  BsaHeader header_;
};

}  // namespace

std::unique_ptr<asset::FileProvider> OpenBsa(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return nullptr;
  BsaHeader header{};
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!file || header.magic != kBsaMagic) {
    REC_ERROR("not a bsa: {}", path);
    return nullptr;
  }
  if (header.version != 104 && header.version != 105) {
    REC_ERROR("unsupported bsa version {} in {}", header.version, path);
    return nullptr;
  }
  REC_INFO("bsa {}: v{}, {} files", path, header.version, header.file_count);
  return std::make_unique<BsaProvider>(path, header);
}

}  // namespace rec::bethesda

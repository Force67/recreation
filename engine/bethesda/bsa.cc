#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "asset/asset_id.h"
#include "bethesda/archive.h"
#include "bethesda/compression.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr u32 kBsaMagic = FourCc('B', 'S', 'A', '\0');

constexpr u32 kFlagIncludeDirNames = 0x01;
constexpr u32 kFlagIncludeFileNames = 0x02;
constexpr u32 kFlagCompressedByDefault = 0x04;
constexpr u32 kFlagEmbedFileNames = 0x100;

constexpr u32 kFileSizeCompressionToggle = 0x40000000;

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

struct FileEntry {
  u32 size = 0;  // bit 30 toggles compression against the archive default
  u32 offset = 0;
};

class BsaProvider final : public asset::FileProvider {
 public:
  BsaProvider(std::string path, BsaHeader header) : path_(std::move(path)), header_(header) {}

  bool Parse() {
    std::ifstream file(path_, std::ios::binary);
    if (!file) return false;
    file.seekg(header_.folder_records_offset);

    // v105: u64 hash, u32 count, u32 pad, u64 offset. v104: u64, u32, u32.
    base::Vector<u32> folder_file_counts(header_.folder_count);
    for (u32& count : folder_file_counts) {
      char buffer[24];
      file.read(buffer, header_.version >= 105 ? 24 : 16);
      std::memcpy(&count, buffer + 8, 4);
    }

    bool dir_names = header_.archive_flags & kFlagIncludeDirNames;
    base::Vector<FileEntry> files;
    base::Vector<u32> file_folder;  // parallel to files, indexes folder_names
    base::Vector<std::string> folder_names;
    files.reserve(header_.file_count);
    folder_names.reserve(header_.folder_count);
    for (u32 i = 0; i < header_.folder_count; ++i) {
      std::string folder_name;
      if (dir_names) {
        u8 length = 0;
        file.read(reinterpret_cast<char*>(&length), 1);
        folder_name.resize(length);
        file.read(folder_name.data(), length);
        while (!folder_name.empty() && folder_name.back() == '\0') folder_name.pop_back();
      }
      folder_names.push_back(asset::NormalizePath(folder_name));
      for (u32 j = 0; j < folder_file_counts[i]; ++j) {
        u64 hash;
        FileEntry entry;
        file.read(reinterpret_cast<char*>(&hash), 8);
        file.read(reinterpret_cast<char*>(&entry.size), 4);
        file.read(reinterpret_cast<char*>(&entry.offset), 4);
        files.push_back(entry);
        file_folder.push_back(i);
      }
    }
    if (!file || !(header_.archive_flags & kFlagIncludeFileNames)) return false;

    // The file name block: file_count zero terminated strings.
    std::string names(header_.total_file_name_length, '\0');
    file.read(names.data(), static_cast<std::streamsize>(names.size()));
    if (!file) return false;
    size_t pos = 0;
    for (size_t i = 0; i < files.size(); ++i) {
      if (pos >= names.size()) return false;
      std::string_view name(names.c_str() + pos);
      pos += name.size() + 1;
      std::string full = folder_names[file_folder[i]];
      if (!full.empty()) full += '/';
      full += asset::NormalizePath(name);
      entries_.emplace(std::move(full), files[i]);
    }
    return true;
  }

  bool Contains(std::string_view normalized_path) const override {
    return entries_.find(std::string(normalized_path)) != entries_.end();
  }

  std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const override {
    auto it = entries_.find(std::string(normalized_path));
    if (it == entries_.end()) return std::nullopt;
    const FileEntry& entry = it->second;

    bool compressed = header_.archive_flags & kFlagCompressedByDefault;
    if (entry.size & kFileSizeCompressionToggle) compressed = !compressed;
    u32 size = entry.size & ~kFileSizeCompressionToggle;

    std::ifstream file(path_, std::ios::binary);
    file.seekg(entry.offset);
    if (header_.archive_flags & kFlagEmbedFileNames) {
      u8 length = 0;
      file.read(reinterpret_cast<char*>(&length), 1);
      file.seekg(length, std::ios::cur);
      size -= 1 + length;
    }
    if (!compressed) {
      base::Vector<u8> data(size);
      file.read(reinterpret_cast<char*>(data.data()), size);
      if (!file) return std::nullopt;
      return data;
    }

    u32 uncompressed_size = 0;
    file.read(reinterpret_cast<char*>(&uncompressed_size), 4);
    base::Vector<u8> compressed_data(size - 4);
    file.read(reinterpret_cast<char*>(compressed_data.data()),
              static_cast<std::streamsize>(compressed_data.size()));
    if (!file) return std::nullopt;
    base::Vector<u8> data(uncompressed_size);
    ByteSpan src(compressed_data.data(), compressed_data.size());
    bool ok = header_.version >= 105 ? Lz4FrameDecompress(src, data.data(), data.size())
                                     : ZlibInflate(src, data.data(), data.size());
    if (!ok) {
      REC_WARN("bsa decompression failed: {} in {}", normalized_path, path_);
      return std::nullopt;
    }
    return data;
  }

  void Enumerate(const std::function<void(std::string_view)>& fn) const override {
    for (const auto& [name, entry] : entries_) fn(name);
  }

  std::string name() const override { return path_; }

 private:
  std::string path_;
  BsaHeader header_;
  // std::string keyed map stays STL, matching the Vfs path convention.
  std::unordered_map<std::string, FileEntry> entries_;
};

}  // namespace

base::UniquePointer<asset::FileProvider> OpenBsa(const std::string& path) {
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
  file.close();
  auto provider = base::MakeUnique<BsaProvider>(path, header);
  if (!provider->Parse()) {
    REC_ERROR("failed to parse bsa tables: {}", path);
    return nullptr;
  }
  REC_INFO("bsa {}: v{}, {} files", path, header.version, header.file_count);
  return provider;
}

}  // namespace rec::bethesda

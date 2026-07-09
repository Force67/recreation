#include "bethesda/material_db.h"

#include <cctype>
#include <cstring>

namespace rx::bethesda {
namespace {

// Component-type ids of the two string fields the reader needs: a TextureSet's
// name and a texture file path. Verified against shipped materialsbeta.cdb.
constexpr u32 kFieldTextureSetName = 500;
constexpr u32 kFieldTextureFile = 906;

std::string ToLowerSlashes(std::string_view raw) {
  std::string out(raw);
  for (char& c : out) {
    if (c == '\\') c = '/';
    else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// "textures/..." key the vfs uses, from a stored "Data\Textures\...DDS" path.
std::string NormalizeTexturePath(std::string_view raw) {
  std::string path = ToLowerSlashes(raw);
  size_t anchor = path.find("textures/");
  return anchor == std::string::npos ? "textures/" + path : path.substr(anchor);
}

// The material stem for a TextureSet object named "<stem>_TextureSet<n>", or
// empty when the name is not a texture set (a composite path, a layer, etc.).
std::string TextureSetStem(std::string_view name) {
  std::string lower = ToLowerSlashes(name);
  size_t marker = lower.rfind("_textureset");
  if (marker == std::string::npos || marker == 0) return {};
  // Only a trailing _TextureSet[digits] qualifies, so a layer that merely
  // contains the substring is not mistaken for the set itself.
  for (size_t i = marker + 11; i < lower.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(lower[i]))) return {};
  }
  return lower.substr(0, marker);
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

void StarfieldMaterialDb::Build(ByteSpan cdb) {
  by_stem_.clear();
  if (cdb.size() < 16) return;

  // The current TextureSet being accumulated. An empty stem means the last
  // fid-500 string was not a named texture set, so its textures are skipped.
  std::string cur_stem;
  Textures cur;
  auto commit = [&]() {
    if (!cur_stem.empty() && !cur.base_color.empty())
      by_stem_.emplace(std::move(cur_stem), std::move(cur));
  };

  size_t p = 16;  // BETH magic + version + two header u32s
  while (p + 8 <= cdb.size()) {
    const u8* chunk = cdb.data() + p;
    bool ascii = true;
    for (int i = 0; i < 4; ++i)
      if (chunk[i] < 32 || chunk[i] >= 127) ascii = false;
    if (!ascii) break;
    u32 size;
    std::memcpy(&size, chunk + 4, 4);
    if (p + 8 + static_cast<size_t>(size) > cdb.size()) break;
    const u8* data = chunk + 8;

    if (std::memcmp(chunk, "DIFF", 4) == 0 && size >= 8) {
      u32 fid;
      std::memcpy(&fid, data, 4);
      if (fid == kFieldTextureSetName || fid == kFieldTextureFile) {
        u16 len;
        std::memcpy(&len, data + 6, 2);
        if (8u + len <= size) {
          std::string_view value(reinterpret_cast<const char*>(data + 8), len);
          if (size_t z = value.find('\0'); z != std::string_view::npos) value = value.substr(0, z);
          if (fid == kFieldTextureSetName) {
            commit();
            cur = Textures{};
            cur_stem = TextureSetStem(value);
          } else if (!cur_stem.empty()) {
            // First texture of each kind in the set is the set's own; a shared
            // placeholder trails the group, so first wins.
            std::string norm = NormalizeTexturePath(value);
            if (cur.base_color.empty() && EndsWith(norm, "_color.dds")) cur.base_color = std::move(norm);
            else if (cur.normal.empty() && EndsWith(norm, "_normal.dds")) cur.normal = std::move(norm);
            else if (cur.emissive.empty() && EndsWith(norm, "_emissive.dds")) cur.emissive = std::move(norm);
          }
        }
      }
    }
    p += 8 + size;
  }
  commit();
}

bool StarfieldMaterialDb::Lookup(std::string_view mat_path, std::string* base_color,
                                 std::string* normal, std::string* emissive) const {
  std::string lower = ToLowerSlashes(mat_path);
  size_t slash = lower.find_last_of('/');
  std::string_view file = slash == std::string::npos ? std::string_view(lower)
                                                     : std::string_view(lower).substr(slash + 1);
  if (EndsWith(file, ".mat")) file = file.substr(0, file.size() - 4);

  auto it = by_stem_.find(std::string(file));
  if (it == by_stem_.end()) return false;
  if (base_color && !it->second.base_color.empty()) *base_color = it->second.base_color;
  if (normal && !it->second.normal.empty()) *normal = it->second.normal;
  if (emissive && !it->second.emissive.empty()) *emissive = it->second.emissive;
  return true;
}

}  // namespace rx::bethesda

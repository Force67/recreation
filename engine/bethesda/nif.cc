#include "bethesda/nif.h"

#include <cstring>

#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr std::string_view kMagic = "Gamebryo File Format, Version ";

}  // namespace

std::optional<NifHeader> ParseNifHeader(ByteSpan data) {
  if (data.size() < 64) return std::nullopt;
  std::string_view text(reinterpret_cast<const char*>(data.data()),
                        std::min<size_t>(data.size(), 64));
  if (!text.starts_with(kMagic)) return std::nullopt;

  size_t newline = text.find('\n');
  if (newline == std::string_view::npos) return std::nullopt;

  NifHeader header;
  // Binary version follows the text line: u32 version, u8 endian, u32 user
  // version, u32 block count, then user version 2 inside the BS header.
  size_t pos = newline + 1;
  if (data.size() < pos + 4) return std::nullopt;
  std::memcpy(&header.version, data.data() + pos, 4);

  // TODO: endian byte, user versions, block type table, block sizes. Needed
  // before ConvertNif can walk blocks.
  return header;
}

std::unique_ptr<asset::Mesh> ConvertNif(ByteSpan data, asset::AssetId id) {
  auto header = ParseNifHeader(data);
  if (!header) {
    REC_WARN("not a nif");
    return nullptr;
  }
  auto mesh = std::make_unique<asset::Mesh>();
  mesh->id = id;
  mesh->lods.emplace_back();
  // TODO: walk BSTriShape blocks, decode the vertex desc (half floats for
  // SSE positions/uvs, FO4 packed normals), bake node transforms, emit one
  // submesh per shape with its shader property as the material reference.
  return mesh;
}

}  // namespace rec::bethesda

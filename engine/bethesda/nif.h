#ifndef RECREATION_BETHESDA_NIF_H_
#define RECREATION_BETHESDA_NIF_H_

#include <optional>
#include <string>
#include <string_view>

#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "asset/material.h"
#include "asset/mesh.h"
#include "core/types.h"

namespace rec::bethesda {

// Gamebryo/NetImmerse scene file. All three games ship 20.2.0.7 with
// different BS stream versions (SSE: 100 with BSTriShape packed vertices
// plus legacy NiTriShape for tree trunks). We do not keep the scene graph,
// geometry is flattened into one engine mesh with node transforms baked in.
// Vertices stay in Bethesda object space (Z-up, game units); the cell
// streamer owns the engine space conversion.
struct NifHeader {
  u32 version = 0;
  u32 user_version = 0;
  u32 bs_version = 0;
  base::Vector<std::string> block_types;
  base::Vector<u16> block_type_index;
  base::Vector<u32> block_sizes;
  base::Vector<u32> block_offsets;  // absolute offsets into the file
};

std::optional<NifHeader> ParseNifHeader(ByteSpan data);

// The flattened result: one mesh with a submesh per shape, plus the
// materials those submeshes reference (synthesized from the NIF shader
// property blocks) and the texture paths the materials want loaded.
struct NifConversion {
  base::UniquePointer<asset::Mesh> mesh;
  base::Vector<asset::Material> materials;
  // Normalized vfs paths ("textures/..."), deduplicated.
  base::Vector<std::string> texture_paths;
  u32 skipped_shapes = 0;  // skinned/strip/empty shapes we cannot use yet
};

NifConversion ConvertNifScene(ByteSpan data, asset::AssetId id, std::string_view source_path);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_NIF_H_

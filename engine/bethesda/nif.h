#ifndef RECREATION_BETHESDA_NIF_H_
#define RECREATION_BETHESDA_NIF_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asset/mesh.h"
#include "core/types.h"

namespace rec::bethesda {

// Gamebryo/NetImmerse scene file. All three games ship 20.2.0.7 with
// different user versions (SSE: BSTriShape with packed vertices, FO4/76:
// their own vertex desc). We do not keep the scene graph, geometry is
// flattened into engine meshes with node transforms baked in.
struct NifHeader {
  u32 version = 0;
  u32 user_version = 0;
  u32 user_version_2 = 0;
  u32 block_count = 0;
  std::vector<std::string> block_types;
};

std::optional<NifHeader> ParseNifHeader(ByteSpan data);

// Converts the geometry blocks (BSTriShape, NiTriShape) into an engine mesh.
// Shader property blocks (BSLightingShaderProperty, BSEffectShaderProperty)
// become material references resolved by the material converter.
std::unique_ptr<asset::Mesh> ConvertNif(ByteSpan data, asset::AssetId id);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_NIF_H_

#ifndef RECREATION_ASSET_MATERIALX_H_
#define RECREATION_ASSET_MATERIALX_H_

#include <string>

#include "asset/material.h"

namespace rec::asset {

// Loads the constant inputs of the first standard_surface node in a MaterialX
// (.mtlx) document into a Material, mapping the OpenPBR-style inputs (base,
// metalness, specular_roughness, coat, sheen, subsurface, transmission, thin
// film, emission, ...) onto the engine's pbr lobes. Node-graph connected inputs
// (no constant value) are skipped. Returns false on a read/parse error; out is
// left at its defaults for inputs the document omits.
bool LoadMaterialX(const std::string& path, Material* out);

}  // namespace rec::asset

#endif  // RECREATION_ASSET_MATERIALX_H_

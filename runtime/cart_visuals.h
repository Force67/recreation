#ifndef RECREATION_RUNTIME_CART_VISUALS_H_
#define RECREATION_RUNTIME_CART_VISUALS_H_

#include "asset/asset_database.h"
#include "asset/mesh.h"
#include "core/math.h"
#include "render/core/renderer.h"

// Art shared by the two things in the engine that put a horse-drawn cart on
// screen: the free-rolling carriage (carriage.cc) and the Helgen intro cutscene
// (helgen_intro.cc). The game's own cart NIF when the data is mounted, plus
// engine-drawn wheels and graybox parts so both still run with no game data.
namespace rx::cart {

// The Skyrim fast-travel carriage body (CartFurniture, 0x00090048).
inline constexpr const char* kBodyMesh = "meshes/furniture/cart/cartfurnstatic01.nif";

// Bakes a Bethesda-space cart NIF into engine space (axis swap + metre scale)
// under `id`. With `recenter` the vertices are moved onto the mesh bounds
// centre, so a transform at the chassis centre places it; without, the NIF's
// authored origin is kept, which is what a caller placing seats in the cart's
// own local space wants. False when the mesh is not in the archives.
bool BakeBody(asset::AssetDatabase* assets, render::Renderer* renderer, const char* path,
              bool recenter, const char* id, asset::AssetId* out);

// An X-axis cylinder (a wheel: axle along local +X, disc in the Y-Z plane), so a
// transform whose right axis is X spins it about the axle.
asset::Mesh MakeWheel(render::Renderer* renderer, const char* id, f32 radius, f32 half_width,
                      bool upload);

// A flat-shaded box with one material, for graybox parts.
asset::Mesh MakeBox(render::Renderer* renderer, const char* id, Vec3 half, f32 r, f32 g, f32 b,
                    bool upload);

}  // namespace rx::cart

#endif  // RECREATION_RUNTIME_CART_VISUALS_H_

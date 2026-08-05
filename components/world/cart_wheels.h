#ifndef RECREATION_WORLD_CART_WHEELS_H_
#define RECREATION_WORLD_CART_WHEELS_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "asset/mesh.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::world {

// Splitting a cart's art into a body and the wheels that turn under it.
//
// A wagon ships as one mesh with its wheels welded into the body: Bethesda's
// carriage is a single "Carriage01" shape cut up by material, with no wheel node
// to drive, because in the original game a carriage never actually rolls. So the
// only way to turn them is to find them in the geometry, and a wheel is the one
// unmistakable shape in a cart: an island of triangles that is thin across one
// axis and round about it.
//
// Mesh space throughout, whatever units the source mesh is in.
struct CartWheel {
  Vec3 hub{};      // where the wheel turns, in the source mesh's space
  f32 radius = 0;  // its radius about the hub
  asset::Mesh mesh;
};

struct CartParts {
  asset::Mesh body;                  // everything that is not a wheel
  base::Vector<CartWheel> wheels;    // hub-centred, ordered left-front first
  bool split() const { return wheels.size() >= 2; }
};

// Pulls the wheels out of `source`. The pieces are given asset ids built from
// `name`: `name` itself for the body and `name` + "/wheel0".. for the wheels.
// Returns a `CartParts` with no wheels (and an untouched body) when the mesh
// holds nothing wheel-shaped, which is the caller's cue to keep drawing the
// original.
//
// `spin_axis` is the axis a wheel is thin across (the axle): 0 = x, and for a
// Bethesda cart converted into engine space that is the one to use.
CartParts SplitCartWheels(const asset::Mesh& source, const base::String& name, u32 spin_axis = 0);

}  // namespace rx::world

#endif  // RECREATION_WORLD_CART_WHEELS_H_

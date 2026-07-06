#ifndef RECREATION_PHYSICS_SHAPE_DESC_H_
#define RECREATION_PHYSICS_SHAPE_DESC_H_

// Engine-neutral collision shape description: a tree of primitives,
// convex hulls and placed children that PhysicsWorld lowers into Jolt
// shapes. Producers include the Havok .hkx decoder (authored Bethesda
// collision/ragdolls) and, eventually, NIF bhk blocks; nothing in here is
// format-specific.

#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace rec::physics {

struct ShapeDesc {
  enum class Kind { kSphere, kCapsule, kBox, kConvexHull, kCompound, kPlaced, kInvalid };
  Kind kind = Kind::kInvalid;
  f32 radius = 0;       // sphere; capsule
  Vec3 a{}, b{};        // capsule segment ends (local space)
  Vec3 half_extents{};  // box
  std::vector<Vec3> vertices;      // convex hull
  std::vector<ShapeDesc> children;  // compound members / the placed child
  // kPlaced child placement: four float4 COLUMNS (basis c0, c1, c2, origin).
  f32 transform[16] = {};
};

}  // namespace rec::physics

#endif  // RECREATION_PHYSICS_SHAPE_DESC_H_

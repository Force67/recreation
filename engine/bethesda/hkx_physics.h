#ifndef RECREATION_BETHESDA_HKX_PHYSICS_H_
#define RECREATION_BETHESDA_HKX_PHYSICS_H_

// Class-level decoding of the physics-relevant Havok 2010.2 types found in
// Skyrim SE packfiles (skeleton.hkx ragdolls first): hkaSkeleton,
// hkpPhysicsData/System, hkpRigidBody with its shape hierarchy, and the
// ragdoll / limited-hinge constraints. Member offsets are the serialized
// 64-bit hk2010 layouts, verified against the vanilla files (see hkxinfo's
// annotated hex mode); everything decodes into engine-neutral structs the
// Jolt bridge consumes.
//
// Units: Skyrim's ragdoll data is authored in game units (1 unit = 1.428 cm),
// matching the NIF geometry. Values are passed through raw; the physics
// bridge applies the engine's unit scale.

#include <optional>
#include <string>
#include <vector>

#include "bethesda/hkx.h"
#include "core/math.h"

namespace rx::bethesda {

struct HkxBone {
  std::string name;
  i16 parent = -1;  // index into the skeleton's bones, -1 = root
  Vec3 translation{};
  f32 rotation[4] = {0, 0, 0, 1};  // x,y,z,w reference pose (parent space)
  f32 scale = 1.0f;
};

struct HkxSkeleton {
  std::string name;
  std::vector<HkxBone> bones;
};

struct HkxShape {
  enum class Kind { kSphere, kCapsule, kBox, kConvexVertices, kList, kTransform, kUnknown };
  Kind kind = Kind::kUnknown;
  std::string class_name;  // original havok class, for diagnostics
  f32 radius = 0;          // sphere/capsule (and convex margin)
  Vec3 a{}, b{};           // capsule segment ends
  Vec3 half_extents{};     // box
  std::vector<Vec3> vertices;      // convex hull
  std::vector<HkxShape> children;  // list / transform
  // kTransform child placement: four float4 COLUMNS (basis c0, c1, c2,
  // origin), i.e. havok hkTransform memory order.
  f32 transform[16] = {};
};

struct HkxRigidBody {
  std::string name;
  HkxShape shape;
  Vec3 position{};
  f32 rotation[4] = {0, 0, 0, 1};
  f32 mass = 0;  // 0 = fixed/keyframed
  f32 friction = 0.5f;
  f32 restitution = 0.4f;
  u8 motion_type = 0;  // hkpMotion::MotionType (1 dynamic .. 5 fixed)
  u64 object_offset = 0;  // packfile offset, keys constraint endpoints
};

struct HkxConstraint {
  enum class Kind { kRagdoll, kLimitedHinge, kOther };
  Kind kind = Kind::kOther;
  std::string name;
  i32 body_a = -1;  // indices into HkxPhysics::bodies
  i32 body_b = -1;
  // Pivot frames in each body's local space (row-major 3x3 basis + origin).
  f32 frame_a[12] = {};
  f32 frame_b[12] = {};
  // Ragdoll: twist about the primary axis, cone around it, plane wobble.
  f32 twist_min = 0, twist_max = 0;
  f32 cone_max = 0;
  f32 plane_min = 0, plane_max = 0;
  // Limited hinge: rotation range about the hinge axis.
  f32 hinge_min = 0, hinge_max = 0;
};

struct HkxRagdoll {
  std::vector<i32> bone_to_body;  // ragdoll-skeleton bone -> body index (-1 none)
  i32 skeleton = -1;              // index into HkxPhysics::skeletons
};

struct HkxPhysics {
  std::vector<HkxSkeleton> skeletons;
  std::vector<HkxRigidBody> bodies;
  std::vector<HkxConstraint> constraints;
  std::optional<HkxRagdoll> ragdoll;
};

// Walks the packfile's object table and decodes everything physics-shaped.
// Returns data even when only some classes are present (a skeleton-only file
// yields just skeletons).
HkxPhysics DecodePhysics(const HkxFile& hkx);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_HKX_PHYSICS_H_

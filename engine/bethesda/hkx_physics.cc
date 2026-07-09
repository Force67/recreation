#include "bethesda/hkx_physics.h"

#include <cmath>
#include <unordered_map>

#include "core/log.h"

namespace rx::bethesda {
namespace {

// Serialized hk2010 64-bit member offsets, verified against the vanilla SE
// skeleton.hkx with hkxinfo --hex (fixup annotations pin the pointers, float
// patterns pin the transforms). hkReferencedObject occupies the first 16
// bytes of every class.
namespace off {
// hkaSkeleton
constexpr u64 kSkeletonName = 0x10;
constexpr u64 kSkeletonParents = 0x18;   // hkArray<i16>
constexpr u64 kSkeletonBones = 0x28;     // hkArray<hkaBone{name*, bool}> (16B)
constexpr u64 kSkeletonRefPose = 0x38;   // hkArray<hkQsTransform> (48B)
// hkpPhysicsData
constexpr u64 kPhysicsDataSystems = 0x18;  // hkArray<hkpPhysicsSystem*>
// hkpPhysicsSystem
constexpr u64 kSystemRigidBodies = 0x10;  // hkArray<hkpRigidBody*>
constexpr u64 kSystemConstraints = 0x20;  // hkArray<hkpConstraintInstance*>
// hkpRigidBody
constexpr u64 kBodyShape = 0x20;      // collidable.shape
constexpr u64 kBodyName = 0xB0;
constexpr u64 kBodyFriction = 0xD4;
constexpr u64 kBodyRestitution = 0xD8;
constexpr u64 kBodyMotionType = 0x14A;
constexpr u64 kBodyPosition = 0x1A0;  // motionState.transform translation
constexpr u64 kBodyRotation = 0x1D0;  // sweptTransform rotation0 (quat)
constexpr u64 kBodyInvMass = 0x22C;   // inertiaAndInvMass.w
// Convex shapes
constexpr u64 kShapeRadius = 0x20;         // hkpConvexShape::radius
constexpr u64 kCapsuleA = 0x30;            // float4 (w = radius)
constexpr u64 kCapsuleB = 0x40;
constexpr u64 kBoxHalfExtents = 0x30;
constexpr u64 kConvexRotatedVerts = 0x50;  // hkArray<hkFourTransposedPoints>
constexpr u64 kConvexNumVerts = 0x60;
// hkpListShape / hkpTransformShape / hkpMoppBvTreeShape
constexpr u64 kListChildren = 0x30;     // hkArray<ChildInfo{shape*, u32, u32, u32}> (24B?)
// hkpConvexTransform/TranslateShape: convex radius +0x20, child shape ptr
// +0x30, then the placement (full hkTransform / translate float4) at +0x40
// (verified: dragon wing membranes).
constexpr u64 kCvxTransformChild = 0x30;
constexpr u64 kCvxTransformXf = 0x40;
// hkpMoppBvTreeShape: code* at +0x20, then mopp data blob members; the
// hkpSingleShapeContainer's shape pointer sits at +0x58 (verified: dragon
// skeleton wing membranes).
constexpr u64 kMoppChild = 0x58;
// hkpConstraintInstance
constexpr u64 kConstraintData = 0x18;
constexpr u64 kConstraintEntityA = 0x28;
constexpr u64 kConstraintEntityB = 0x30;
constexpr u64 kConstraintName = 0x50;
// hkpRagdollConstraintData atoms (relative to the data object)
constexpr u64 kRagdollXfA = 0x30;   // hkTransform (3 basis columns + origin)
constexpr u64 kRagdollXfB = 0x70;
constexpr u64 kRagdollTwistMin = 0x134;
constexpr u64 kRagdollTwistMax = 0x138;
constexpr u64 kRagdollConeMax = 0x14C;
constexpr u64 kRagdollPlaneMin = 0x15C;
constexpr u64 kRagdollPlaneMax = 0x160;
// hkpLimitedHingeConstraintData atoms (transforms atom matches the ragdoll's;
// the angLimit atom carries {hdr u32, minAngle, maxAngle, tapFrames})
constexpr u64 kHingeXfA = 0x30;
constexpr u64 kHingeXfB = 0x70;
constexpr u64 kHingeMin = 0xE8;
constexpr u64 kHingeMax = 0xEC;
// hkaRagdollInstance
constexpr u64 kRagdollBodies = 0x10;
constexpr u64 kRagdollConstraints = 0x20;
constexpr u64 kRagdollBoneMap = 0x30;  // hkArray<i32>
constexpr u64 kRagdollSkeleton = 0x40;
}  // namespace off

Vec3 ReadVec3(const HkxFile& hkx, u64 at) {
  return {hkx.F32(at), hkx.F32(at + 4), hkx.F32(at + 8)};
}

void ReadTransform12(const HkxFile& hkx, u64 at, f32 out[12]) {
  // hkTransform: three basis COLUMN vectors then the origin, each a float4.
  // Stored row-major here as basis rows for the consumer: out[r*4+c].
  for (u32 col = 0; col < 4; ++col) {
    for (u32 row = 0; row < 3; ++row) {
      out[row * 4 + col] = hkx.F32(at + col * 16 + row * 4);
    }
  }
}

HkxShape DecodeShape(const HkxFile& hkx, u64 at, u32 depth) {
  HkxShape shape;
  if (at == HkxFile::kNull || depth > 8) return shape;
  std::string_view cls = hkx.class_of(at);
  shape.class_name = std::string(cls);

  if (cls == "hkpCapsuleShape") {
    shape.kind = HkxShape::Kind::kCapsule;
    shape.radius = hkx.F32(at + off::kShapeRadius);
    shape.a = ReadVec3(hkx, at + off::kCapsuleA);
    shape.b = ReadVec3(hkx, at + off::kCapsuleB);
  } else if (cls == "hkpSphereShape") {
    shape.kind = HkxShape::Kind::kSphere;
    shape.radius = hkx.F32(at + off::kShapeRadius);
  } else if (cls == "hkpBoxShape") {
    shape.kind = HkxShape::Kind::kBox;
    shape.radius = hkx.F32(at + off::kShapeRadius);
    shape.half_extents = ReadVec3(hkx, at + off::kBoxHalfExtents);
  } else if (cls == "hkpConvexVerticesShape") {
    shape.kind = HkxShape::Kind::kConvexVertices;
    shape.radius = hkx.F32(at + off::kShapeRadius);
    u32 blocks = 0;
    u64 verts = hkx.Array(at + off::kConvexRotatedVerts, &blocks);
    u32 count = hkx.U32(at + off::kConvexNumVerts);
    // hkFourTransposedPoints: 3 float4 rows carrying x0..x3 / y0..y3 / z0..z3.
    for (u32 blk = 0; blk < blocks && verts != HkxFile::kNull; ++blk) {
      u64 base = verts + static_cast<u64>(blk) * 48;
      for (u32 i = 0; i < 4; ++i) {
        if (shape.vertices.size() >= count) break;
        shape.vertices.push_back({hkx.F32(base + i * 4), hkx.F32(base + 16 + i * 4),
                                  hkx.F32(base + 32 + i * 4)});
      }
    }
  } else if (cls == "hkpListShape") {
    shape.kind = HkxShape::Kind::kList;
    u32 count = 0;
    u64 children = hkx.Array(at + off::kListChildren, &count);
    // ChildInfo: shape*, collisionFilterInfo, shapeSize, numChildShapes +
    // padding = 32 bytes (verified: dragon skeleton list of 8).
    constexpr u64 kChildStride = 32;
    for (u32 i = 0; i < count && children != HkxFile::kNull; ++i) {
      u64 child = hkx.Pointer(children + i * kChildStride);
      if (child != HkxFile::kNull) shape.children.push_back(DecodeShape(hkx, child, depth + 1));
    }
  } else if (cls == "hkpConvexTransformShape" || cls == "hkpConvexTranslateShape") {
    shape.kind = HkxShape::Kind::kTransform;
    u64 child = hkx.Pointer(at + off::kCvxTransformChild);
    if (child != HkxFile::kNull) shape.children.push_back(DecodeShape(hkx, child, depth + 1));
    if (cls == "hkpConvexTransformShape") {
      // Four float4 columns: basis c0,c1,c2 then the origin.
      for (u32 i = 0; i < 16; ++i) shape.transform[i] = hkx.F32(at + off::kCvxTransformXf + i * 4);
    } else {
      // Translate-only: identity basis + stored offset.
      shape.transform[0] = shape.transform[5] = shape.transform[10] = 1.0f;
      for (u32 i = 0; i < 3; ++i) {
        shape.transform[12 + i] = hkx.F32(at + off::kCvxTransformXf + i * 4);
      }
      shape.transform[15] = 1.0f;
    }
  } else if (cls == "hkpMoppBvTreeShape") {
    // The mopp code is an acceleration structure over its child; Jolt builds
    // its own BVH, so only the child matters.
    u64 child = hkx.Pointer(at + off::kMoppChild);
    if (child != HkxFile::kNull) return DecodeShape(hkx, child, depth + 1);
  }
  return shape;
}

HkxSkeleton DecodeSkeleton(const HkxFile& hkx, u64 at) {
  HkxSkeleton skeleton;
  skeleton.name = std::string(hkx.CString(at + off::kSkeletonName));
  u32 parent_count = 0, bone_count = 0, pose_count = 0;
  u64 parents = hkx.Array(at + off::kSkeletonParents, &parent_count);
  u64 bones = hkx.Array(at + off::kSkeletonBones, &bone_count);
  u64 pose = hkx.Array(at + off::kSkeletonRefPose, &pose_count);
  for (u32 i = 0; i < bone_count; ++i) {
    HkxBone bone;
    bone.name = std::string(hkx.CString(bones + static_cast<u64>(i) * 16));
    if (i < parent_count && parents != HkxFile::kNull) bone.parent = hkx.I16(parents + i * 2);
    if (i < pose_count && pose != HkxFile::kNull) {
      u64 t = pose + static_cast<u64>(i) * 48;  // hkQsTransform: T, Q, S float4s
      bone.translation = ReadVec3(hkx, t);
      for (u32 c = 0; c < 4; ++c) bone.rotation[c] = hkx.F32(t + 16 + c * 4);
      bone.scale = hkx.F32(t + 32);
    }
    skeleton.bones.push_back(std::move(bone));
  }
  return skeleton;
}

}  // namespace

HkxPhysics DecodePhysics(const HkxFile& hkx) {
  HkxPhysics physics;
  std::unordered_map<u64, i32> body_index;   // packfile offset -> bodies index
  std::unordered_map<u64, i32> skeleton_index;

  // Bodies and skeletons first so constraints/ragdolls can reference them.
  for (const HkxObject& obj : hkx.objects()) {
    if (obj.class_name == "hkaSkeleton") {
      skeleton_index[obj.offset] = static_cast<i32>(physics.skeletons.size());
      physics.skeletons.push_back(DecodeSkeleton(hkx, obj.offset));
    } else if (obj.class_name == "hkpRigidBody") {
      HkxRigidBody body;
      body.object_offset = obj.offset;
      body.name = std::string(hkx.CString(obj.offset + off::kBodyName));
      body.friction = hkx.F32(obj.offset + off::kBodyFriction);
      body.restitution = hkx.F32(obj.offset + off::kBodyRestitution);
      body.motion_type = hkx.U8(obj.offset + off::kBodyMotionType);
      body.position = ReadVec3(hkx, obj.offset + off::kBodyPosition);
      for (u32 c = 0; c < 4; ++c) {
        body.rotation[c] = hkx.F32(obj.offset + off::kBodyRotation + c * 4);
      }
      f32 inv_mass = hkx.F32(obj.offset + off::kBodyInvMass);
      body.mass = inv_mass > 1e-6f ? 1.0f / inv_mass : 0.0f;
      u64 shape = hkx.Pointer(obj.offset + off::kBodyShape);
      if (shape != HkxFile::kNull) body.shape = DecodeShape(hkx, shape, 0);
      body_index[obj.offset] = static_cast<i32>(physics.bodies.size());
      physics.bodies.push_back(std::move(body));
    }
  }

  auto body_of = [&](u64 pointer_slot) {
    u64 target = hkx.Pointer(pointer_slot);
    auto it = body_index.find(target);
    return it == body_index.end() ? -1 : it->second;
  };

  // Vanilla files serialize every constraint TWICE - one full instance+data
  // pair under the physics system and an identical pair under the ragdoll
  // instance - so walking all hkpConstraintInstance objects double-counts.
  // The physics system's constraint array is the authoritative set; fall
  // back to the flat object walk only when no system exists.
  std::vector<u64> constraint_offsets;
  for (const HkxObject& obj : hkx.objects()) {
    if (obj.class_name != "hkpPhysicsSystem") continue;
    u32 count = 0;
    u64 items = hkx.Array(obj.offset + off::kSystemConstraints, &count);
    for (u32 i = 0; i < count && items != HkxFile::kNull; ++i) {
      u64 instance = hkx.Pointer(items + static_cast<u64>(i) * 8);
      if (instance != HkxFile::kNull) constraint_offsets.push_back(instance);
    }
  }
  if (constraint_offsets.empty()) {
    for (const HkxObject& obj : hkx.objects()) {
      if (obj.class_name == "hkpConstraintInstance") constraint_offsets.push_back(obj.offset);
    }
  }
  for (u64 instance : constraint_offsets) {
    {
      const HkxObject obj{instance, hkx.class_of(instance)};
      HkxConstraint constraint;
      constraint.name = std::string(hkx.CString(obj.offset + off::kConstraintName));
      constraint.body_a = body_of(obj.offset + off::kConstraintEntityA);
      constraint.body_b = body_of(obj.offset + off::kConstraintEntityB);
      u64 data = hkx.Pointer(obj.offset + off::kConstraintData);
      if (data != HkxFile::kNull) {
        std::string_view cls = hkx.class_of(data);
        if (cls == "hkpRagdollConstraintData") {
          constraint.kind = HkxConstraint::Kind::kRagdoll;
          ReadTransform12(hkx, data + off::kRagdollXfA, constraint.frame_a);
          ReadTransform12(hkx, data + off::kRagdollXfB, constraint.frame_b);
          constraint.twist_min = hkx.F32(data + off::kRagdollTwistMin);
          constraint.twist_max = hkx.F32(data + off::kRagdollTwistMax);
          constraint.cone_max = hkx.F32(data + off::kRagdollConeMax);
          constraint.plane_min = hkx.F32(data + off::kRagdollPlaneMin);
          constraint.plane_max = hkx.F32(data + off::kRagdollPlaneMax);
        } else if (cls == "hkpLimitedHingeConstraintData") {
          constraint.kind = HkxConstraint::Kind::kLimitedHinge;
          ReadTransform12(hkx, data + off::kHingeXfA, constraint.frame_a);
          ReadTransform12(hkx, data + off::kHingeXfB, constraint.frame_b);
          constraint.hinge_min = hkx.F32(data + off::kHingeMin);
          constraint.hinge_max = hkx.F32(data + off::kHingeMax);
        }
      }
      physics.constraints.push_back(std::move(constraint));
    }
  }

  for (const HkxObject& obj : hkx.objects()) {
    if (obj.class_name != "hkaRagdollInstance") continue;
    HkxRagdoll ragdoll;
    u32 map_count = 0;
    u64 map = hkx.Array(obj.offset + off::kRagdollBoneMap, &map_count);
    for (u32 i = 0; i < map_count && map != HkxFile::kNull; ++i) {
      i32 havok_body = static_cast<i32>(hkx.U32(map + i * 4));
      // The map indexes the ragdoll instance's own body array; ours is the
      // global decode order, which matches for vanilla single-system files.
      ragdoll.bone_to_body.push_back(havok_body);
    }
    u64 skeleton = hkx.Pointer(obj.offset + off::kRagdollSkeleton);
    auto it = skeleton_index.find(skeleton);
    ragdoll.skeleton = it == skeleton_index.end() ? -1 : it->second;
    physics.ragdoll = std::move(ragdoll);
  }
  return physics;
}

}  // namespace rx::bethesda

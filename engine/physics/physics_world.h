#ifndef RECREATION_PHYSICS_PHYSICS_WORLD_H_
#define RECREATION_PHYSICS_PHYSICS_WORLD_H_

#include <functional>
#include <memory>

#include "asset/mesh.h"
#include "core/math.h"
#include "core/types.h"
#include "physics/shape_desc.h"

namespace rec::physics {

// Opaque body handle; 0 is invalid.
using BodyId = u64;
// Opaque character-controller handle; 0 is invalid.
using CharacterId = u64;
// Opaque joint/constraint handle; 0 is invalid.
using JointId = u64;

// Jolt-backed rigid body world. Fixed-step simulation driven from the sim
// stage; dynamic bodies report their transforms back for ECS sync. Bodies
// below a water surface get buoyancy and drag impulses each step (the Jolt
// boat/water sample scheme), with the surface height supplied per position
// so streamed worlds and flat demo sheets share one path.
class PhysicsWorld {
 public:
  // Returns true with the surface height and flow velocity when `position`
  // is over water. Flow drags floating bodies (rivers carry them).
  using WaterHeightFn = std::function<bool(const Vec3& position, f32* height, Vec3* flow)>;

  PhysicsWorld();
  ~PhysicsWorld();

  PhysicsWorld(const PhysicsWorld&) = delete;
  PhysicsWorld& operator=(const PhysicsWorld&) = delete;

  bool Initialize();
  void Update(f32 dt);

  void set_water_height(WaterHeightFn fn) { water_height_ = std::move(fn); }

  // Static colliders.
  BodyId AddStaticBox(const Vec3& position, const Vec3& half_extent);
  BodyId AddStaticMesh(const asset::Mesh& mesh, const Vec3& position, const f32 rotation[4],
                       f32 scale);
  // Shared-shape path for streamed instances: the mesh bakes once per key,
  // every placement reuses it through a scale wrapper.
  bool RegisterMeshShape(u64 key, const asset::Mesh& mesh);
  bool has_mesh_shape(u64 key) const;
  BodyId AddStaticMeshInstance(u64 key, const Vec3& position, const f32 rotation[4], f32 scale);
  // Heightfield grid of sample*sample values covering size x size meters,
  // anchored at `origin` (min corner). For streamed terrain cells.
  BodyId AddHeightField(const Vec3& origin, const f32* heights, u32 samples, f32 size);

  // Generic shape-tree bodies (authored collision from the havok decoder or
  // other producers). `scale` converts the desc's units into meters (pass
  // the game-unit scale for Bethesda data). Dynamic bodies take an explicit
  // mass in kg; 0 falls back to Jolt's density-derived mass.
  BodyId AddStaticShape(const ShapeDesc& desc, const Vec3& position, const f32 rotation[4],
                        f32 scale);
  // filter_group/subgroup: bodies sharing a filter group collide unless the
  // pair of subgroups was disabled - how a ragdoll's jointed limbs overlap
  // at the hips/shoulders without fighting their constraints.
  BodyId AddDynamicShape(const ShapeDesc& desc, const Vec3& position, const f32 rotation[4],
                         f32 scale, f32 mass, f32 friction, f32 restitution,
                         i32 filter_group = -1, u32 subgroup = 0);
  i32 CreateBodyFilterGroup(u32 subgroup_count);
  void DisableFilterPair(i32 group, u32 sub_a, u32 sub_b);

  // Ragdoll joints between two dynamic bodies. Frames are 3x4 row-major
  // (basis rows + origin column) in each body's LOCAL space, column 0 = the
  // twist/hinge axis, column 1 = the plane/normal axis, in desc units
  // (scaled by `scale` like the shapes). Angles in radians. The swing-twist
  // joint approximates Havok's asymmetric plane limit with a symmetric one.
  // Return a JointId handle (0 on failure) that the motor/pose API below
  // addresses; also usable as a bool for the plain "did it stick" check.
  JointId AddSwingTwistJoint(BodyId a, BodyId b, const f32 frame_a[12], const f32 frame_b[12],
                             f32 scale, f32 twist_min, f32 twist_max, f32 cone_max,
                             f32 plane_min, f32 plane_max);
  JointId AddHingeJoint(BodyId a, BodyId b, const f32 frame_a[12], const f32 frame_b[12], f32 scale,
                        f32 angle_min, f32 angle_max);

  // Powered-ragdoll motor drive (the "physical hit reaction" primitive). A
  // joint's motor is switched to position mode so the constraint is pulled
  // toward a target relative orientation by a critically-tunable spring
  // (`frequency` in Hz, `damping` ~1.0 for no overshoot). For swing-twist
  // joints both the swing and twist motors are driven; for hinges the single
  // hinge motor. Off by default, so unpowered ragdolls (plain drops) are
  // unaffected.
  void EnableJointMotors(JointId joint, f32 frequency, f32 damping);
  // Sets the motor target as a CONSTRAINT-SPACE orientation quaternion
  // (x,y,z,w): the rotation q that takes body A's constraint frame to body
  // B's, i.e. the pose the drive holds. For swing-twist this is passed to
  // Jolt's SetTargetOrientationCS; for hinges the twist angle about the hinge
  // axis is extracted from the quaternion and used as the target angle. Feed
  // GetJointOrientation()'s spawn-time reading here to hold the bind pose.
  void SetJointMotorTarget(JointId joint, const f32 target_quat[4]);
  // Reads a joint's CURRENT relative orientation in constraint space as a
  // quaternion (x,y,z,w); hinges report a pure rotation about the hinge axis
  // by their current angle. Used to snapshot the bind-pose target and to
  // measure motor tracking error. False on an invalid handle.
  bool GetJointOrientation(JointId joint, f32 out_quat[4]) const;

  // Applies an instantaneous impulse (kg*m/s) at a body's centre of mass and
  // wakes it. Drives the "get hit" disturbance for the powered-ragdoll test.
  void ApplyImpulse(BodyId id, const Vec3& impulse);
  // Switches a body to kinematic (infinite mass, immovable, immune to
  // gravity/forces) while keeping its layer and collision group. Used to pin
  // a ragdoll's root so the figure hangs from it like a puppet.
  void SetBodyKinematic(BodyId id);

  // Dynamic bodies; density in kg/m^3 (wood floats, stone sinks).
  BodyId AddDynamicBox(const Vec3& position, const Vec3& half_extent, f32 density,
                       const Vec3& initial_velocity);
  BodyId AddDynamicSphere(const Vec3& position, f32 radius, f32 density,
                          const Vec3& initial_velocity);

  void RemoveBody(BodyId id);

  // Kinematic capsule: a solid body that never falls or tips and is driven by
  // SetBodyPosition each tick. Used for NPCs and remote players so the local
  // player's character controller collides with them (they block / get shoved)
  // while their authoritative position comes from animation / replication.
  BodyId AddKinematicCapsule(const Vec3& position, f32 radius, f32 half_height);
  // Teleports a body to a new pose (rotation is x,y,z,w). For the kinematic
  // capsules above, called every tick from the entity's transform.
  void SetBodyPosition(BodyId id, const Vec3& position, const f32 rotation[4]);

  // Kinematic character controller (Jolt CharacterVirtual): a capsule that
  // walks slopes/stairs. `position` is the capsule centre. 0 is invalid.
  CharacterId CreateCharacter(const Vec3& position, f32 radius, f32 half_height);
  // Steps the controller: `horizontal_velocity` is the desired ground-plane
  // velocity (m/s); gravity and jumping are handled internally. Returns the new
  // capsule-centre position and whether it is on the ground.
  void MoveCharacter(CharacterId id, const Vec3& horizontal_velocity, bool jump, f32 dt,
                     Vec3* out_position, bool* out_grounded);
  void SetCharacterPosition(CharacterId id, const Vec3& position);

  // Closest hit of a ray; used by foot IK to find the ground under a foot.
  struct RayHit {
    Vec3 position;
    Vec3 normal{0, 1, 0};
    f32 distance = 0;
  };
  bool Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance, RayHit* out) const;

  // Pose of a (dynamic) body for ECS sync.
  bool GetBodyTransform(BodyId id, Vec3* position, f32 rotation[4]) const;

  u32 dynamic_body_count() const { return dynamic_count_; }
  bool initialized() const { return impl_ != nullptr; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  WaterHeightFn water_height_;
  u32 dynamic_count_ = 0;
};

}  // namespace rec::physics

#endif  // RECREATION_PHYSICS_PHYSICS_WORLD_H_

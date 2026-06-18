#ifndef RECREATION_PHYSICS_PHYSICS_WORLD_H_
#define RECREATION_PHYSICS_PHYSICS_WORLD_H_

#include <functional>
#include <memory>

#include "asset/mesh.h"
#include "core/math.h"
#include "core/types.h"

namespace rec::physics {

// Opaque body handle; 0 is invalid.
using BodyId = u64;

// Jolt-backed rigid body world. Fixed-step simulation driven from the sim
// stage; dynamic bodies report their transforms back for ECS sync. Bodies
// below a water surface get buoyancy and drag impulses each step (the Jolt
// boat/water sample scheme), with the surface height supplied per position
// so streamed worlds and flat demo sheets share one path.
class PhysicsWorld {
 public:
  // Returns true and the surface height when `position` is over water.
  using WaterHeightFn = std::function<bool(const Vec3& position, f32* height)>;

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
  // Heightfield grid of sample*sample values covering size x size meters,
  // anchored at `origin` (min corner). For streamed terrain cells.
  BodyId AddHeightField(const Vec3& origin, const f32* heights, u32 samples, f32 size);

  // Dynamic bodies; density in kg/m^3 (wood floats, stone sinks).
  BodyId AddDynamicBox(const Vec3& position, const Vec3& half_extent, f32 density,
                       const Vec3& initial_velocity);
  BodyId AddDynamicSphere(const Vec3& position, f32 radius, f32 density,
                          const Vec3& initial_velocity);

  void RemoveBody(BodyId id);

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

// Built when Jolt is unavailable; every call is a no-op.
#include "physics/physics_world.h"

namespace rec::physics {

struct PhysicsWorld::Impl {};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;
bool PhysicsWorld::Initialize() { return false; }
void PhysicsWorld::Update(f32) {}
BodyId PhysicsWorld::AddStaticBox(const Vec3&, const Vec3&) { return 0; }
BodyId PhysicsWorld::AddStaticMesh(const asset::Mesh&, const Vec3&, const f32[4], f32) {
  return 0;
}
BodyId PhysicsWorld::AddHeightField(const Vec3&, const f32*, u32, f32) { return 0; }
bool PhysicsWorld::RegisterMeshShape(u64, const asset::Mesh&) { return false; }
bool PhysicsWorld::has_mesh_shape(u64) const { return false; }
BodyId PhysicsWorld::AddStaticMeshInstance(u64, const Vec3&, const f32[4], f32) { return 0; }
BodyId PhysicsWorld::AddDynamicBox(const Vec3&, const Vec3&, f32, const Vec3&) { return 0; }
BodyId PhysicsWorld::AddDynamicSphere(const Vec3&, f32, f32, const Vec3&) { return 0; }
void PhysicsWorld::RemoveBody(BodyId) {}
bool PhysicsWorld::GetBodyTransform(BodyId, Vec3*, f32[4]) const { return false; }

}  // namespace rec::physics

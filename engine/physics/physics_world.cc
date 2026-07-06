#include "physics/physics_world.h"

#include <cstdarg>
#include <cstdio>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <base/containers/unordered_map.h>

#include "core/log.h"

namespace rec::physics {
namespace {

namespace layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kDynamic = 1;
constexpr u32 kCount = 2;
}  // namespace layers

namespace broad {
constexpr JPH::BroadPhaseLayer kStatic{0};
constexpr JPH::BroadPhaseLayer kDynamic{1};
constexpr u32 kCount = 2;
}  // namespace broad

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return broad::kCount; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return layer == layers::kStatic ? broad::kStatic : broad::kDynamic;
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    return layer == broad::kStatic ? "static" : "dynamic";
  }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_layer) const override {
    // Statics never collide with the static broadphase.
    if (layer == layers::kStatic) return broad_layer == broad::kDynamic;
    return true;
  }
};

class ObjectLayerPair final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    return a == layers::kDynamic || b == layers::kDynamic;
  }
};

void TraceCallback(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  REC_INFO("jolt: {}", buffer);
}

JPH::Vec3 ToJolt(const Vec3& v) { return {v.x, v.y, v.z}; }

}  // namespace

struct PhysicsWorld::Impl {
  BroadPhaseLayers broad_phase_layers;
  ObjectVsBroadPhase object_vs_broad_phase;
  ObjectLayerPair object_layer_pair;
  std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
  std::unique_ptr<JPH::JobSystemThreadPool> job_system;
  std::unique_ptr<JPH::PhysicsSystem> system;
  base::Vector<JPH::BodyID> dynamic_bodies;
  base::UnorderedMap<u64, JPH::RefConst<JPH::Shape>> mesh_shapes;
  struct CharacterEntry {
    JPH::Ref<JPH::CharacterVirtual> character;
    f32 vy = 0;  // tracked vertical velocity (gravity + jump)
  };
  base::Vector<CharacterEntry> characters;
};

PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld() {
  if (impl_) {
    impl_.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
}

bool PhysicsWorld::Initialize() {
  JPH::RegisterDefaultAllocator();
  JPH::Trace = TraceCallback;
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();

  impl_ = std::make_unique<Impl>();
  impl_->temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
  impl_->job_system = std::make_unique<JPH::JobSystemThreadPool>(
      JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
      static_cast<int>(std::thread::hardware_concurrency()) - 1);

  impl_->system = std::make_unique<JPH::PhysicsSystem>();
  impl_->system->Init(65536, 0, 65536, 10240, impl_->broad_phase_layers,
                      impl_->object_vs_broad_phase, impl_->object_layer_pair);
  impl_->system->SetGravity({0, -9.81f, 0});
  REC_INFO("jolt physics initialized");
  return true;
}

void PhysicsWorld::Update(f32 dt) {
  if (!impl_) return;

  // Buoyancy before the step, the Jolt water sample scheme: any awake
  // dynamic body below its local water surface gets buoyancy + drag.
  if (water_height_) {
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    for (JPH::BodyID id : impl_->dynamic_bodies) {
      JPH::RVec3 position = bodies.GetCenterOfMassPosition(id);
      f32 surface = 0;
      Vec3 flow{};
      if (!water_height_({static_cast<f32>(position.GetX()), static_cast<f32>(position.GetY()),
                          static_cast<f32>(position.GetZ())},
                         &surface, &flow)) {
        continue;
      }
      // Moving water never lets floaters sleep; still water does.
      bool in_flow = (flow.x != 0 || flow.y != 0 || flow.z != 0) && position.GetY() < surface;
      if (!bodies.IsActive(id)) {
        if (!in_flow) continue;
        bodies.ActivateBody(id);
      }
      JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), id);
      if (!lock.Succeeded()) continue;
      JPH::Body& body = lock.GetBody();
      body.ApplyBuoyancyImpulse(JPH::RVec3(position.GetX(), surface, position.GetZ()),
                                JPH::Vec3::sAxisY(), 1.2f, 0.5f, 0.05f, ToJolt(flow),
                                impl_->system->GetGravity(), dt);
    }
  }

  impl_->system->Update(dt, 1, impl_->temp_allocator.get(), impl_->job_system.get());
}

BodyId PhysicsWorld::AddStaticBox(const Vec3& position, const Vec3& half_extent) {
  if (!impl_) return 0;
  JPH::BodyCreationSettings settings(new JPH::BoxShape(ToJolt(half_extent)), ToJolt(position),
                                     JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                     layers::kStatic);
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddStaticMesh(const asset::Mesh& mesh, const Vec3& position,
                                   const f32 rotation[4], f32 scale) {
  if (!impl_ || mesh.lods.empty()) return 0;
  const asset::MeshLod& lod = mesh.lods[0];
  if (lod.indices.size() < 3) return 0;

  JPH::VertexList vertices;
  vertices.reserve(lod.vertices.size());
  for (const asset::Vertex& v : lod.vertices) {
    vertices.push_back({v.position[0], v.position[1], v.position[2]});
  }
  JPH::IndexedTriangleList triangles;
  triangles.reserve(lod.indices.size() / 3);
  for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
    triangles.push_back({lod.indices[i], lod.indices[i + 1], lod.indices[i + 2], 0});
  }
  JPH::Ref<JPH::ShapeSettings> shape_settings =
      new JPH::MeshShapeSettings(std::move(vertices), std::move(triangles));
  if (scale != 1.0f) {
    shape_settings = new JPH::ScaledShapeSettings(shape_settings, JPH::Vec3::sReplicate(scale));
  }
  JPH::ShapeSettings::ShapeResult result = shape_settings->Create();
  if (result.HasError()) {
    REC_WARN("mesh collider failed: {}", result.GetError().c_str());
    return 0;
  }
  JPH::BodyCreationSettings settings(result.Get(), ToJolt(position),
                                     JPH::Quat(rotation[0], rotation[1], rotation[2], rotation[3]),
                                     JPH::EMotionType::Static, layers::kStatic);
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

bool PhysicsWorld::RegisterMeshShape(u64 key, const asset::Mesh& mesh) {
  if (!impl_ || mesh.lods.empty()) return false;
  if (impl_->mesh_shapes.find(key)) return true;
  const asset::MeshLod& lod = mesh.lods[0];
  if (lod.indices.size() < 3) return false;

  JPH::VertexList vertices;
  vertices.reserve(lod.vertices.size());
  for (const asset::Vertex& v : lod.vertices) {
    vertices.push_back({v.position[0], v.position[1], v.position[2]});
  }
  JPH::IndexedTriangleList triangles;
  triangles.reserve(lod.indices.size() / 3);
  for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
    triangles.push_back({lod.indices[i], lod.indices[i + 1], lod.indices[i + 2], 0});
  }
  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
  JPH::ShapeSettings::ShapeResult result = settings.Create();
  if (result.HasError()) return false;
  impl_->mesh_shapes.insert(key, result.Get());
  return true;
}

bool PhysicsWorld::has_mesh_shape(u64 key) const {
  return impl_ && impl_->mesh_shapes.find(key) != nullptr;
}

BodyId PhysicsWorld::AddStaticMeshInstance(u64 key, const Vec3& position, const f32 rotation[4],
                                           f32 scale) {
  if (!impl_) return 0;
  const JPH::RefConst<JPH::Shape>* shape = impl_->mesh_shapes.find(key);
  if (!shape) return 0;
  JPH::RefConst<JPH::Shape> instance = *shape;
  if (scale != 1.0f) {
    instance = new JPH::ScaledShape(instance, JPH::Vec3::sReplicate(scale));
  }
  JPH::BodyCreationSettings settings(instance, ToJolt(position),
                                     JPH::Quat(rotation[0], rotation[1], rotation[2], rotation[3]),
                                     JPH::EMotionType::Static, layers::kStatic);
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddHeightField(const Vec3& origin, const f32* heights, u32 samples,
                                    f32 size) {
  if (!impl_ || samples < 2) return 0;
  JPH::HeightFieldShapeSettings shape(heights, ToJolt(origin),
                                      {size / static_cast<f32>(samples - 1), 1.0f,
                                       size / static_cast<f32>(samples - 1)},
                                      samples);
  JPH::ShapeSettings::ShapeResult result = shape.Create();
  if (result.HasError()) {
    REC_WARN("heightfield collider failed: {}", result.GetError().c_str());
    return 0;
  }
  JPH::BodyCreationSettings settings(result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Static, layers::kStatic);
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

namespace {

// Quaternion from three orthonormal basis COLUMNS (c0, c1, c2), the layout
// ShapeDesc::transform carries.
JPH::Quat QuatFromColumns(const f32* t) {
  JPH::Mat44 m = JPH::Mat44::sIdentity();
  m.SetColumn3(0, {t[0], t[1], t[2]});
  m.SetColumn3(1, {t[4], t[5], t[6]});
  m.SetColumn3(2, {t[8], t[9], t[10]});
  return m.GetQuaternion().Normalized();
}

// Lowers a ShapeDesc tree into a Jolt shape, scaling all metrics into
// meters. Returns null on unconvertible input (empty hulls, degenerate
// primitives), which callers surface as a failed body.
JPH::Ref<JPH::Shape> BuildShape(const rec::physics::ShapeDesc& desc, f32 scale) {
  using Kind = rec::physics::ShapeDesc::Kind;
  switch (desc.kind) {
    case Kind::kSphere: {
      if (desc.radius * scale < 1e-4f) return nullptr;
      return new JPH::SphereShape(desc.radius * scale);
    }
    case Kind::kCapsule: {
      // Havok capsules are arbitrary segments; Jolt's are Y-centered, so
      // wrap in a rotated-translated placement.
      JPH::Vec3 a(desc.a.x, desc.a.y, desc.a.z);
      JPH::Vec3 b(desc.b.x, desc.b.y, desc.b.z);
      a *= scale;
      b *= scale;
      f32 radius = desc.radius * scale;
      f32 half_height = (b - a).Length() * 0.5f;
      if (radius < 1e-4f) return nullptr;
      if (half_height < 1e-4f) return new JPH::SphereShape(radius);
      JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(half_height, radius);
      JPH::Vec3 axis = (b - a).Normalized();
      JPH::Quat align = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis);
      JPH::RotatedTranslatedShapeSettings placed((a + b) * 0.5f, align, capsule);
      auto result = placed.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kBox: {
      JPH::Vec3 half(desc.half_extents.x, desc.half_extents.y, desc.half_extents.z);
      half *= scale;
      f32 min_extent = half.ReduceMin();
      if (min_extent < 1e-4f) return nullptr;
      return new JPH::BoxShape(half, JPH::min(0.05f, min_extent * 0.5f));
    }
    case Kind::kConvexHull: {
      if (desc.vertices.size() < 4) return nullptr;
      JPH::Array<JPH::Vec3> points;
      points.reserve(desc.vertices.size());
      for (const auto& v : desc.vertices) points.emplace_back(v.x * scale, v.y * scale,
                                                              v.z * scale);
      JPH::ConvexHullShapeSettings hull(points);
      auto result = hull.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kCompound: {
      JPH::StaticCompoundShapeSettings compound;
      u32 added = 0;
      for (const auto& child : desc.children) {
        if (child.kind == Kind::kPlaced && !child.children.empty()) {
          JPH::Ref<JPH::Shape> inner = BuildShape(child.children[0], scale);
          if (!inner) continue;
          JPH::Vec3 origin(child.transform[12], child.transform[13], child.transform[14]);
          compound.AddShape(origin * scale, QuatFromColumns(child.transform), inner);
          ++added;
        } else if (JPH::Ref<JPH::Shape> inner = BuildShape(child, scale)) {
          compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), inner);
          ++added;
        }
      }
      if (added == 0) return nullptr;
      if (added == 1 && desc.children.size() == 1 &&
          desc.children[0].kind != Kind::kPlaced) {
        return BuildShape(desc.children[0], scale);  // trivial list
      }
      auto result = compound.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kPlaced: {
      if (desc.children.empty()) return nullptr;
      JPH::Ref<JPH::Shape> inner = BuildShape(desc.children[0], scale);
      if (!inner) return nullptr;
      JPH::Vec3 origin(desc.transform[12], desc.transform[13], desc.transform[14]);
      JPH::RotatedTranslatedShapeSettings placed(origin * scale, QuatFromColumns(desc.transform),
                                                 inner);
      auto result = placed.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kInvalid:
      return nullptr;
  }
  return nullptr;
}

}  // namespace

BodyId PhysicsWorld::AddStaticShape(const ShapeDesc& desc, const Vec3& position,
                                    const f32 rotation[4], f32 scale) {
  if (!impl_) return 0;
  JPH::Ref<JPH::Shape> shape = BuildShape(desc, scale);
  if (!shape) return 0;
  JPH::Quat rot(rotation[0], rotation[1], rotation[2], rotation[3]);
  if (rot.LengthSq() < 1e-6f) rot = JPH::Quat::sIdentity();
  JPH::BodyCreationSettings settings(shape, ToJolt(position), rot.Normalized(),
                                     JPH::EMotionType::Static, layers::kStatic);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddDynamicShape(const ShapeDesc& desc, const Vec3& position,
                                     const f32 rotation[4], f32 scale, f32 mass, f32 friction,
                                     f32 restitution) {
  if (!impl_) return 0;
  JPH::Ref<JPH::Shape> shape = BuildShape(desc, scale);
  if (!shape) return 0;
  JPH::Quat rot(rotation[0], rotation[1], rotation[2], rotation[3]);
  if (rot.LengthSq() < 1e-6f) rot = JPH::Quat::sIdentity();
  JPH::BodyCreationSettings settings(shape, ToJolt(position), rot.Normalized(),
                                     JPH::EMotionType::Dynamic, layers::kDynamic);
  settings.mFriction = friction;
  settings.mRestitution = restitution;
  if (mass > 0.0f) {
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;
  }
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(id);
  ++dynamic_count_;
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddDynamicBox(const Vec3& position, const Vec3& half_extent, f32 density,
                                   const Vec3& initial_velocity) {
  if (!impl_) return 0;
  JPH::Ref<JPH::BoxShape> shape = new JPH::BoxShape(ToJolt(half_extent));
  shape->SetDensity(density);
  JPH::BodyCreationSettings settings(shape, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Dynamic, layers::kDynamic);
  settings.mLinearVelocity = ToJolt(initial_velocity);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(id);
  ++dynamic_count_;
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddDynamicSphere(const Vec3& position, f32 radius, f32 density,
                                      const Vec3& initial_velocity) {
  if (!impl_) return 0;
  JPH::Ref<JPH::SphereShape> shape = new JPH::SphereShape(radius);
  shape->SetDensity(density);
  JPH::BodyCreationSettings settings(shape, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Dynamic, layers::kDynamic);
  settings.mLinearVelocity = ToJolt(initial_velocity);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(id);
  ++dynamic_count_;
  return id.GetIndexAndSequenceNumber() + 1;
}

CharacterId PhysicsWorld::CreateCharacter(const Vec3& position, f32 radius, f32 half_height) {
  if (!impl_) return 0;
  JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
  settings->mShape = new JPH::CapsuleShape(half_height, radius);
  // Accept ground contacts on the lower hemisphere so slopes register.
  settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
  JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
      settings, ToJolt(position), JPH::Quat::sIdentity(), 0, impl_->system.get());
  impl_->characters.push_back({character, 0.0f});
  return impl_->characters.size();  // id = index + 1
}

void PhysicsWorld::MoveCharacter(CharacterId id, const Vec3& horizontal_velocity, bool jump,
                                 f32 dt, Vec3* out_position, bool* out_grounded) {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return;
  Impl::CharacterEntry& entry = impl_->characters[id - 1];
  JPH::CharacterVirtual* character = entry.character;

  bool grounded = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
  if (grounded && entry.vy < 0) entry.vy = 0;
  if (grounded && jump) entry.vy = 4.5f;
  entry.vy += impl_->system->GetGravity().GetY() * dt;

  character->SetLinearVelocity({horizontal_velocity.x, entry.vy, horizontal_velocity.z});
  JPH::CharacterVirtual::ExtendedUpdateSettings update;
  character->ExtendedUpdate(dt, impl_->system->GetGravity(), update,
                            impl_->system->GetDefaultBroadPhaseLayerFilter(layers::kDynamic),
                            impl_->system->GetDefaultLayerFilter(layers::kDynamic), {}, {},
                            *impl_->temp_allocator);
  JPH::RVec3 p = character->GetPosition();
  if (out_position) {
    *out_position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
                     static_cast<f32>(p.GetZ())};
  }
  if (out_grounded) {
    *out_grounded = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
  }
}

void PhysicsWorld::SetCharacterPosition(CharacterId id, const Vec3& position) {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return;
  impl_->characters[id - 1].character->SetPosition(ToJolt(position));
}

BodyId PhysicsWorld::AddKinematicCapsule(const Vec3& position, f32 radius, f32 half_height) {
  if (!impl_) return 0;
  JPH::Ref<JPH::CapsuleShape> shape = new JPH::CapsuleShape(half_height, radius);
  // Kinematic (driven by SetBodyPosition, immune to gravity/forces) but in the
  // dynamic layer so the player's character controller collides with it.
  JPH::BodyCreationSettings settings(shape, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Kinematic, layers::kDynamic);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  return id.GetIndexAndSequenceNumber() + 1;
}

void PhysicsWorld::SetBodyPosition(BodyId id, const Vec3& position, const f32 rotation[4]) {
  if (!impl_ || id == 0) return;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  JPH::Quat q(rotation[0], rotation[1], rotation[2], rotation[3]);
  impl_->system->GetBodyInterface().SetPositionAndRotation(body, ToJolt(position), q,
                                                           JPH::EActivation::Activate);
}

void PhysicsWorld::RemoveBody(BodyId id) {
  if (!impl_ || id == 0) return;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  bodies.RemoveBody(body);
  bodies.DestroyBody(body);
  for (size_t i = 0; i < impl_->dynamic_bodies.size(); ++i) {
    if (impl_->dynamic_bodies[i] == body) {
      impl_->dynamic_bodies[i] = impl_->dynamic_bodies.back();
      impl_->dynamic_bodies.pop_back();
      --dynamic_count_;
      break;
    }
  }
}

bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance,
                           RayHit* out) const {
  if (!impl_) return false;
  Vec3 dir = Normalize(direction);
  JPH::RRayCast ray{ToJolt(origin), ToJolt(dir) * max_distance};
  JPH::RayCastResult result;
  if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, result)) return false;
  JPH::RVec3 hit = ray.GetPointOnRay(result.mFraction);
  out->position = {static_cast<f32>(hit.GetX()), static_cast<f32>(hit.GetY()),
                   static_cast<f32>(hit.GetZ())};
  out->distance = result.mFraction * max_distance;
  out->normal = {0, 1, 0};
  JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(), result.mBodyID);
  if (lock.Succeeded()) {
    JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, hit);
    out->normal = {n.GetX(), n.GetY(), n.GetZ()};
  }
  return true;
}

bool PhysicsWorld::GetBodyTransform(BodyId id, Vec3* position, f32 rotation[4]) const {
  if (!impl_ || id == 0) return false;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  JPH::RVec3 p;
  JPH::Quat q;
  bodies.GetPositionAndRotation(body, p, q);
  *position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
               static_cast<f32>(p.GetZ())};
  rotation[0] = q.GetX();
  rotation[1] = q.GetY();
  rotation[2] = q.GetZ();
  rotation[3] = q.GetW();
  return true;
}

}  // namespace rec::physics

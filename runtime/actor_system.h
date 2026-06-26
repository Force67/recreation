#ifndef RECREATION_RUNTIME_ACTOR_SYSTEM_H_
#define RECREATION_RUNTIME_ACTOR_SYSTEM_H_

#include <optional>
#include <string>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "anim/locomotion.h"
#include "anim/pose.h"
#include "asset/asset_database.h"
#include "asset/skeleton.h"
#include "core/math.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "physics/physics_world.h"
#include "render/core/renderer.h"

namespace rec {

namespace bethesda {
class StarfieldMaterialDb;
}

// Owns the engine's skinned, animated characters: the walkable player, the test
// bringup biped, and the per-NPC instances that mirror streamed-in ECS actors.
// Kept engine-side (not ECS components) because the renderer needs the CPU skin
// bindings to build bone palettes. The rest of the engine reaches the player
// through the small query/command surface at the top of the public section.
class ActorSystem {
 public:
  explicit ActorSystem(EngineContext& ctx);

  // --- Player query / command surface used by the other subsystems ---
  bool HasPlayer() const { return player_actor_ >= 0; }
  ecs::Entity PlayerEntity() const;
  // World-space player position from its ECS transform; false if no player.
  bool PlayerWorldPos(Vec3* out) const;
  physics::CharacterId PlayerCharacter() const;
  f32 PlayerCapsuleOffset() const;
  f32 PlayerYaw() const;  // facing of the player biped, radians about engine up
  // Teleports the player (capsule + ECS transform); the target of a quest MoveTo.
  void TeleportPlayer(f32 x, f32 y, f32 z);
  // Drives the player capsule from walk input: faces `yaw` while moving, steps
  // the character controller, mirrors the capsule into the entity transform, and
  // returns the body (feet) position for the follow camera.
  void MovePlayer(const Vec3& velocity, bool jump, f32 yaw, bool moving, f32 speed, f32 dt,
                  Vec3* out_body);
  // Sets a streamed NPC instance's render gait (planar speed; yaw when moving).
  void SetNpcGait(ecs::Entity npc, f32 speed, bool set_yaw, f32 yaw);

  // --- Spawning ---
  bool SpawnPlayerActor(const Vec3& pos);
  void MaybeSpawnWorldPlayer(const Vec3& ground_pos);
  bool CreateSkyrimActor();
  void CreateTestCharacter();

  // --- Per-frame ---
  void Update(f32 dt);                      // advance gaits + bone matrices
  void EmitDraws(render::FrameView& view);  // append skinned draws + palettes
  void SyncNpcActors();                     // add/remove NPC actor instances
  void SyncSolidBodies();                   // kinematic capsules for NPCs/players

 private:
  // One part of an actor: a skinned mesh sharing the skeleton pose, or a rigid
  // mesh (head, hair) riding a single bone.
  struct ActorPart {
    asset::AssetId mesh;
    asset::SkinBinding skin;
    base::Vector<i32> remap;  // skin bone -> skeleton bone index
    i32 attach_bone = -1;
    Mat4 attach_inverse_bind = Mat4::Identity();
  };
  struct Actor {
    ecs::Entity entity;
    asset::Skeleton skeleton;
    anim::Locomotion locomotion;
    anim::SkeletonPose pose;
    base::Vector<Mat4> bone_model;  // model-space per skeleton bone
    base::Vector<ActorPart> parts;
    bool animate = true;  // false = hold the bind pose
    f32 speed = 0;        // planar speed feeding the gait
    Mat4 skeleton_to_local = Mat4::Identity();  // skeleton space -> entity local
    Mat4 prev_model = Mat4::Identity();
    bool foot_ik = false;
    Vec3 ik_up{0, 1, 0};
    Vec3 ik_forward{0, 0, 1};
    f32 ankle_height = 0.02f;
    physics::CharacterId character = 0;
    f32 yaw = 0;             // facing, radians about engine up (+Y)
    f32 capsule_offset = 0;  // entity origin to capsule centre, along up
  };

  bool LoadActorTemplate(Actor* out);
  bool LoadStarfieldActorTemplate(Actor* out);
  void LoadBuiltinActorTemplate(Actor* out);
  bool LoadActorPart(const std::string& path, Actor& actor, i32 attach_bone = -1);
  bool LoadStarfieldActorPart(const std::string& path, Actor& actor,
                              const bethesda::StarfieldMaterialDb& mat_db);
  base::Vector<std::string> FindHeadPartModels(u32 part_type, u32 max);
  void UpdateOneActor(Actor& actor, f32 dt);
  void EmitOneActor(Actor& actor, render::FrameView& view);

  EngineContext& ctx_;
  ecs::World& world_;
  render::Renderer& renderer_;
  physics::PhysicsWorld& physics_;
  FlyCamera& camera_;
  const EngineConfig& config_;
  asset::Vfs& vfs_;
  bethesda::RecordStore& records_;

  base::Vector<Actor> actors_;
  i32 player_actor_ = -1;  // index into actors_ the walk mode drives, -1 = none
  std::optional<Actor> npc_template_;
  base::UnorderedMap<u64, Actor> npc_actors_;
  base::Vector<u64> scratch_dead_actors_;
  base::UnorderedMap<u64, physics::BodyId> solid_bodies_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_ACTOR_SYSTEM_H_

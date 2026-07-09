#ifndef RECREATION_RUNTIME_ACTOR_SYSTEM_H_
#define RECREATION_RUNTIME_ACTOR_SYSTEM_H_

#include <optional>
#include <string>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include <memory>
#include <unordered_map>

#include <kinema/kinema.h>

#include "anim/locomotion.h"
#include "bethesda/animation_data.h"
#include "bethesda/hkx_anim.h"
#include "bethesda/hkx_physics.h"
#include "anim/pose.h"
#include "asset/asset_database.h"
#include "asset/skeleton.h"
#include "core/math.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "face.h"
#include "physics/physics_world.h"
#include "render/core/renderer.h"

namespace rx {

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
  // A decoded Havok clip: the spline animation plus the per-track remap into
  // the actor's (NIF) skeleton, resolved by bone name through the Havok
  // skeleton the animation was authored against. Shared immutably so
  // template-copied NPCs reuse one decode; playback time lives per actor.
  struct HavokClip {
    bethesda::HkxAnimation animation;
    base::Vector<i32> track_to_skeleton;
    // Root motion + trigger events from the animationdata sidecars (Skyrim
    // strips Havok's extracted motion from the .hkx files). has_motion is
    // false when no block matched the clip.
    bethesda::AnimMotion motion;
    bool has_motion = false;
    std::vector<bethesda::ClipEvent> events;
    // Transcoded kinema blob (uniform quantized keys): the fast runtime
    // sampling path; the spline data above stays as the RX_KINEMA=0
    // fallback and decode-time reference.
    kinema::OwnedClip kinema;
  };
  // A behavior project's animation data: the parsed sidecar text files plus
  // the hkbCharacterStringData animation list (creature clip ids index it).
  struct ProjectAnimData {
    bethesda::AnimationData data;
    std::vector<std::string> animation_names;
  };
  struct Actor {
    ecs::Entity entity;
    asset::Skeleton skeleton;
    anim::Locomotion locomotion;
    anim::SkeletonPose pose;
    std::shared_ptr<const HavokClip> havok_clip;  // when set, replaces the gait
    f32 havok_time = 0;
    // Debug clip cycling (RX_ANIM_B): a preloaded second clip the bringup actor
    // alternates with every 4s, blended across the switch by the inertializer so
    // the pose never pops. Both clips stay resident; the timer swaps which is
    // active. Kinema-only (skipped under RX_KINEMA=0).
    std::shared_ptr<const HavokClip> havok_clip_b;
    bool clip_cycle = false;   // RX_ANIM_B armed
    f32 clip_timer = 0;        // seconds since the last switch
    kinema::Inertializer inert;    // decaying pose offset across a clip switch
    anim::SkeletonPose inert_from;  // scratch: old clip's pose at the switch
    anim::SkeletonPose inert_to;    // scratch: new clip's pose at its start
    // Additive layer (RX_ANIM_ADDITIVE): a second clip composed on top of the
    // base pose each tick (rot compose, trans add, scale multiply) over its own
    // mapped bones. Loops on its own time accumulator. Kinema-only.
    std::shared_ptr<const HavokClip> additive_clip;
    f32 additive_time = 0;
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
    // Strand-hair groom riding the head bone (0 = none). hair_bone/hair_inv are
    // the head bone + its inverse bind, so EmitOneActor can re-derive the head
    // transform each frame and feed it to the groom.
    u32 hair_groom = 0;
    i32 hair_bone = -1;
    Mat4 hair_inverse_bind = Mat4::Identity();
  };

  // soldier_kind: 0 = bare civilian body, 1 = imperial-side soldier (worn
  // cuirass in the body slot), 2 = stormcloak-side soldier.
  bool LoadActorTemplate(Actor* out, int soldier_kind = 0);
  // Plays a spline-compressed .hkx clip on the actor (replacing the
  // procedural gait). Resolves tracks to bones through the character
  // skeleton.hkx (cached). False when the file is missing or undecodable.
  bool CreateCreatureActor(const std::string& name, const std::string& clip_override);
  bool PlayHavokClip(Actor& actor, const std::string& animation_path,
                     const std::string& skeleton_hkx_path, const std::string& actor_name);
  // Decodes + transcodes a clip against the actor's skeleton without touching
  // playback state (PlayHavokClip is this plus assign-and-play). Used to preload
  // the clip-cycle / additive layers. Null on a missing or unmatched file.
  std::shared_ptr<HavokClip> LoadHavokClip(const Actor& actor, const std::string& animation_path,
                                           const std::string& skeleton_hkx_path,
                                           const std::string& actor_name);
  // Samples a clip at `time` and maps its tracks into skeleton-bone space (bind
  // pose for untouched bones), through the same kinema/spline paths as the tick.
  void SampleHavokClipToPose(const Actor& actor, const HavokClip& clip, f32 time,
                             anim::SkeletonPose* out);
  const bethesda::HkxSkeleton* LoadHavokSkeleton(const std::string& skeleton_hkx_path);
  // Cached animationdata sidecars for an actor folder ("character", "troll").
  const ProjectAnimData* LoadProjectAnimData(const std::string& actor_name);
  // Root motion + events for one animation file, resolved through the project
  // data (clip-name match, animation-list index, then unique duration), each
  // gated on the motion duration agreeing with the decoded animation.
  void ResolveClipMotion(const ProjectAnimData& project, const std::string& animation_path,
                         HavokClip* clip);
  // Lazily builds + caches the worn-armour template for a battle side (team 1
  // imperial, team 2 stormcloak), falling back to the bare body template.
  const Actor* SoldierTemplate(int team);
  bool LoadStarfieldActorTemplate(Actor* out);
  void LoadBuiltinActorTemplate(Actor* out);
  bool LoadActorPart(const std::string& path, Actor& actor, i32 attach_bone = -1);
  // Attaches head-part meshes riding the head bone. With a valid `npc` it
  // assembles + morphs that NPC's FaceGen head (face/eyes/brows/beard/hair);
  // otherwise (player, soldiers) it falls back to the default male head + hair.
  void AttachHead(Actor& actor, bethesda::GlobalFormId npc, bool allow_groom = true);
  // Builds a strand groom from a hair nif and rides it on the head bone. Replaces
  // the flat card hair when RX_STRAND_HAIR is on. No-op if the nif has no usable
  // geometry.
  void AttachHairGroom(Actor& actor, const std::string& hair_model, const Vec3& tint,
                       i32 head_bone, const Mat4& inverse_bind);
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
  // Cached havok skeletons by path (the animation track name source; one per
  // creature rig) and a per-tick sampling scratch buffer.
  std::unordered_map<std::string, std::unique_ptr<bethesda::HkxSkeleton>> havok_skeletons_;
  std::vector<bethesda::HkxTrackPose> havok_sample_;
  std::unordered_map<std::string, std::unique_ptr<ProjectAnimData>> project_anim_data_;
  // Kinema sampling scratch (SoA, sized to the widest clip seen this frame).
  std::vector<kinema::Vec3> kinema_t_;
  std::vector<kinema::Quat> kinema_r_;
  std::vector<f32> kinema_s_;

  base::Vector<Actor> actors_;
  i32 player_actor_ = -1;  // index into actors_ the walk mode drives, -1 = none
  std::optional<Actor> npc_template_;
  std::optional<Actor> soldier_templates_[2];  // [0] imperial (team 1), [1] stormcloak (team 2)
  base::UnorderedMap<u64, Actor> npc_actors_;
  base::Vector<u64> scratch_dead_actors_;
  base::UnorderedMap<u64, physics::BodyId> solid_bodies_;
  std::unique_ptr<FaceBuilder> face_builder_;  // lazily built; owns the head caches
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_ACTOR_SYSTEM_H_

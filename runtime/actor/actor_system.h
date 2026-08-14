#ifndef RECREATION_RUNTIME_ACTOR_ACTOR_SYSTEM_H_
#define RECREATION_RUNTIME_ACTOR_ACTOR_SYSTEM_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>
#include <base/optional.h>
#include <base/strings/xstring.h>
#include <kinema/kinema.h>

#include <memory>

#include "anim/locomotion.h"
#include "anim/pose.h"
#include "asset/asset_database.h"
#include "asset/skeleton.h"
#include "components/bethesda/animation_data.h"
#include "components/bethesda/hkx_anim.h"
#include "components/bethesda/hkx_physics.h"
#include "components/bethesda/worn_armor.h"
#include "core/math.h"
#include "ecs/world.h"
#include "physics/physics_world.h"
#include "render/core/renderer.h"
#include "runtime/app/engine_context.h"
#include "runtime/character/face.h"

namespace rx {

namespace bethesda {
class StarfieldMaterialDb;
}

// A compiled, immutable locomotion state machine (idle / walk / run + a 1D
// speed blend space) shared by every actor of one skeleton archetype. Built once
// from real transcoded clips; the per-actor instance/arena live on the Actor.
// Defined in actor_system.cc (holds kinema owning types); actors reference it
// through a shared_ptr, so a forward declaration is all the header needs.
struct LocomotionArchetype;

// First-person weapon policy (runtime/fp_equipment.*). ActorSystem owns one and
// drives it each frame; it in turn commands the FP-rig primitives below.
class FpEquipment;

// Owns the engine's skinned, animated characters: the walkable player, the test
// bringup biped, and the per-NPC instances that mirror streamed-in ECS actors.
// Kept engine-side (not ECS components) because the renderer needs the CPU skin
// bindings to build bone palettes. The rest of the engine reaches the player
// through the small query/command surface at the top of the public section.
class ActorSystem {
 public:
  explicit ActorSystem(EngineContext& ctx);
  ~ActorSystem();  // out-of-line: owns a unique_ptr to the incomplete FpEquipment

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
  // Turns a standing player to face `yaw` (biped convention, +Z faces the
  // heading). MovePlayer only turns the body while it is moving, so a resumed
  // savegame has no other way to say which way the character was looking.
  void SetPlayerFacing(f32 yaw);
  // Sits the player's biped on a piece of furniture: `clip_path` holds the pose
  // and the caller plants it, since Bethesda's seated idles carry the seat as a
  // baked offset on the COM. The locomotion machine stands down until
  // UnseatPlayer, so the body keeps the pose instead of snapping back to idle.
  // False when there is no player or the clip does not load.
  bool SeatPlayer(const base::String& clip_path);
  void UnseatPlayer();
  bool player_seated() const { return player_seated_; }
  // Plants a seated player: `position` is the furniture origin the clip is
  // authored against, `yaw` which way the seat faces.
  void PlaceSeatedPlayer(const Vec3& position, f32 yaw);
  // Mirrors the result of the rx character controller (owned by PlayerController)
  // onto the biped: `feet` position drives skeleton placement, `facing_yaw`
  // (biped +Z faces movement) sets the body rotation while `moving`, `planar_speed`
  // feeds the gait blend, and `grounded` is available for jump/land animation.
  // The capsule itself is stepped by the rx character module, not here.
  void MovePlayer(const Vec3& feet, f32 planar_speed, f32 facing_yaw, bool moving, bool grounded);
  // Sets a streamed NPC instance's render gait (planar speed; yaw when moving).
  void SetNpcGait(ecs::Entity npc, f32 speed, bool set_yaw, f32 yaw);
  // Puts a streamed NPC on one looping clip and hands its world transform to the
  // caller. This is what seats an actor: Bethesda's furniture animations carry the
  // seat as a baked COM offset, so planting the actor on the furniture origin and
  // holding the clip puts them in the chair (or the cart bed). False when the entity
  // has no actor instance or the clip is missing.
  bool PlayNpcClip(ecs::Entity npc, const base::String& clip_path);
  // Puts a streamed actor on its own rig's standing or moving clip, found in the
  // folder its skeleton came from. Lets a caller that drives an animal (the
  // carriage horse) leave which animal it is to the records. False when the
  // entity has no actor instance or the rig ships no such clip.
  bool PlayNpcGait(ecs::Entity npc, bool moving);
  // Whether a streamed NPC entity has a skinned actor instance, i.e. whether it is
  // being drawn at all. What tells a scene director that its cast is really on
  // screen rather than merely present in the ECS.
  bool HasNpcInstance(ecs::Entity npc) const;
  // How many drawable parts a streamed NPC's instance has. 0 means it has an actor
  // but nothing to render, which is invisible rather than missing.
  int NpcInstanceParts(ecs::Entity npc) const;
  // Whether a spawned actor is a person rather than a creature, which is what
  // decides if it can take a seat: a horse standing beside a cart is not a
  // passenger, however close it parks.
  bool NpcIsPerson(ecs::Entity npc) const;
  // World position of an NPC instance's head bone, as of the last pose update.
  // False when the entity has no actor or the rig has no head. What a camera
  // frames a conversation on, rather than guessing at the body's origin.
  bool NpcHeadWorld(ecs::Entity npc, Vec3* out);

  // The head builder, created on first use. A savegame reaches it to override
  // the face its records author, which happens long before any head is built.
  FaceBuilder& faces();

  // --- Spawning ---
  bool SpawnPlayerActor(const Vec3& pos);
  void MaybeSpawnWorldPlayer(const Vec3& ground_pos);
  bool CreateSkyrimActor();
  void CreateTestCharacter();
  // Spawns a creature rig (meshes/actors/<name>/...) as a scripted mover: it
  // renders through the NPC actor path (its walk clip loops to animate the
  // legs) but the caller owns its world position, driving the returned entity's
  // world::Transform each step. Returns a dead entity if the rig data is
  // absent, so callers fall back to a graybox. Used by the carriage horse.
  ecs::Entity SpawnCreatureNpc(const base::String& name,
                               const base::String& clip_override,
                               const Vec3& position,
                               f32 yaw);
  // Spawns a human NPC the caller drives: a full actor wearing `base`'s
  // assembled FaceGen head and holding `clip_path` on a loop. `outfit` is a
  // LoadActorTemplate soldier kind (0 bare, 1 imperial, 2 stormcloak). Like
  // SpawnCreatureNpc the caller owns the entity's world transform and the clip
  // poses the body around it -- including whatever offset the clip was authored
  // with, which is how a furniture animation seats an actor. Used by the Helgen
  // intro to fill the cart with the game's own cart-prisoner idles. Returns a
  // dead entity when the body assets are unavailable.
  ecs::Entity SpawnScriptedNpc(bethesda::GlobalFormId base,
                               const base::String& clip_path,
                               const Vec3& position,
                               f32 yaw,
                               int outfit = 0);

  // --- Per-frame ---
  void Update(f32 dt);                      // advance gaits + bone matrices
  void EmitDraws(render::FrameView& view);  // append skinned draws + palettes
  void SyncNpcActors();                     // add/remove NPC actor instances
  void SyncSolidBodies();                   // kinematic capsules for NPCs/players

  // --- First-person weapon rig primitives (driven by FpEquipment) ---
  // The one-handed first-person clip set (meshes/actors/character/_1stperson/
  // animations/1hm_*.hkx). Idle loops; the rest play once (FpClipDone() latches).
  enum class FpClip { kIdle, kEquip, kUnequip, kAttack };
  // Lazily loads the _1stperson skeleton + arm/hand meshes + the 1hm clip set the
  // first time a weapon is drawn. False (cached) when the assets are missing.
  bool EnsureFpRig();
  // Attaches a weapon hand mesh (already uploaded, e.g. an ItemDef::world_mesh)
  // to the rig's WEAPON node; a 0 id clears it.
  void SetFpWeapon(asset::AssetId mesh);
  void ClearFpWeapon();
  void PlayFpClip(FpClip clip);  // (re)starts a clip on the FP rig
  bool FpClipDone() const { return fp_clip_done_; }
  // Roots the FP skeleton to the camera so the arms sit in the lower frame.
  void SetFpRootView(const Vec3& eye, const Vec3& target);
  // engaged = advance the pose this frame; visible = emit draws + hide the TP
  // player body. Set once per frame by FpEquipment.
  void SetFpFlags(bool engaged, bool visible);

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
    base::Vector<bethesda::ClipEvent> events;
    // Transcoded kinema blob (uniform quantized keys): the fast runtime
    // sampling path; the spline data above stays as the RX_KINEMA=0
    // fallback and decode-time reference.
    kinema::OwnedClip kinema;
  };
  // The three Gamebryo .kf clips a Fallout 3 / New Vegas actor walks around on.
  // Shared immutably so every NPC instanced from the template reuses one decode;
  // playback time lives per actor.
  struct KfLocomotion {
    asset::AnimationClip idle;
    asset::AnimationClip walk;
    asset::AnimationClip run;
  };
  // A behavior project's animation data: the parsed sidecar text files plus
  // the hkbCharacterStringData animation list (creature clip ids index it).
  struct ProjectAnimData {
    bethesda::AnimationData data;
    base::Vector<base::String> animation_names;
  };
  struct Actor {
    ecs::Entity entity;
    asset::Skeleton skeleton;
    anim::Locomotion locomotion;
    anim::SkeletonPose pose;
    std::shared_ptr<const HavokClip> havok_clip;  // when set, replaces the gait
    f32 havok_time = 0;
    // Gamebryo .kf locomotion (Fallout 3 / New Vegas, which ship no Havok
    // clips). Selected by speed and sampled straight into the pose; takes over
    // from the procedural gait when present.
    std::shared_ptr<const KfLocomotion> kf_loco;
    f32 kf_time = 0;
    // Locomotion state machine (RX_KINEMA path): the shared archetype (idle /
    // walk / run + 1D speed blend space + inertialized transitions) plus this
    // actor's own instance/arena/foot-sync. When bound it drives the pose from
    // the actor's planar speed and takes precedence over havok_clip/procedural.
    // Null = the actor stays on the direct-clip or procedural gait path.
    std::shared_ptr<const LocomotionArchetype> loco_arch;
    kinema::StateMachineInstance loco_sm;
    kinema::PoseArena loco_arena;
    kinema::SyncGroup loco_sync;
    bool loco_synced = false;       // foot-sync usable (walk/run share footfall markers)
    base::Vector<f32> loco_params;  // [0]=speed (m/s), [1]=phase [0,1)
    f32 loco_prev_phase = 0;        // last frame's normalized locomotion phase
    f32 loco_phase = 0;             // plain phase accumulator when not foot-synced
    // RX_ANIM_B: a debug driver that scripts the locomotion speed through
    // idle -> walk -> run so the bringup scene exercises the machine's
    // inertialized transitions (replaces the old hand-rolled clip-cycle dance).
    bool loco_debug_drive = false;
    f32 loco_debug_t = 0;
    // Apply the machine's root motion to the entity transform (showcase/bringup
    // actors). False for capsule-driven gameplay actors, whose position is owned
    // by the character controller; the machine only poses them.
    bool loco_apply_root = false;
    // Additive layer (RX_ANIM_ADDITIVE): a clip baked into a kinema additive
    // (delta) clip in skeleton space at load time, composed onto the base pose
    // each tick with kinema::ApplyAdditive. Loops on its own accumulator.
    // Kinema-only; additive_clip keeps the decode (duration/logging).
    std::shared_ptr<const HavokClip> additive_clip;
    std::shared_ptr<kinema::OwnedClip> additive_baked;
    f32 additive_time = 0;
    base::Vector<Mat4> bone_model;  // model-space per skeleton bone
    base::Vector<ActorPart> parts;
    bool animate = true;  // false = hold the bind pose
    // The actors/<folder> this rig's skeleton and clips live under, which is
    // what a clip played on it must be resolved against.
    base::String anim_project = "character";
    // The hand-rolled procedural gait (anim::Locomotion) is authored against the
    // builtin biped's bones. A real game skeleton with no clip holds its bind pose
    // instead: running the procedural pose on a Bethesda rig lays the body out flat.
    bool procedural_gait = false;
    // The caller owns this actor's world position (a scripted mover, e.g. the
    // carriage horse): the looping clip still animates the legs in place, but
    // its extracted root motion is not integrated into the entity transform.
    bool external_position = false;
    f32 speed = 0;                              // planar speed feeding the gait
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

  // Builds the shared NPC rig template on first use (the body every streamed and
  // scripted NPC is instanced from). False when no body assets could be loaded.
  bool EnsureNpcTemplate();
  // soldier_kind: 0 = bare civilian body, 1 = imperial-side soldier (worn
  // cuirass in the body slot), 2 = stormcloak-side soldier. `skip_slots` is a
  // biped slot mask (bethesda::BipedSlotBit) of the bare parts to leave off
  // because worn armour is going to cover them; skin under a cuirass is not
  // just invisible, it z-fights through it.
  bool LoadActorTemplate(Actor* out, int soldier_kind = 0, u32 skip_slots = 0);
  // The armour an actor has equipped, resolved through ARMO -> ARMA -> model
  // against the actor's own race. `covered` comes back as the biped slots the
  // set fills. Empty when the bindings are not up or nothing is worn.
  base::Vector<bethesda::WornArmor> ResolveEquippedArmor(bethesda::GlobalFormId npc_base,
                                                         bethesda::GlobalFormId actor_ref,
                                                         u32* covered);
  // Loads each resolved piece as a skinned part sharing the actor's skeleton.
  // Returns how many went on.
  u32 AttachWornArmor(Actor& actor, base::Span<const bethesda::WornArmor> worn);
  // Plays a spline-compressed .hkx clip on the actor (replacing the
  // procedural gait). Resolves tracks to bones through the character
  // skeleton.hkx (cached). False when the file is missing or undecodable.
  bool CreateCreatureActor(const base::String& name, const base::String& clip_override);
  // Loads a creature's skeleton + skinned body + a looping clip into `out`
  // without creating an entity; the shared rig-load behind CreateCreatureActor
  // and SpawnCreatureNpc.
  bool LoadCreatureRig(const base::String& name, const base::String& clip_override, Actor* out);
  // The creature rig a placed actor's race calls for: the race form and the rig
  // folder ("horse"). False when the race walks on the human character skeleton
  // or names no skeleton at all. Read from the race's own skeleton model (RACE
  // ANAM), so it covers whatever a game or a mod ships rather than a list of
  // known creatures.
  bool CreatureRigForBase(bethesda::GlobalFormId base_npc,
                          bethesda::GlobalFormId* race,
                          base::String* rig) const;
  // The shared rig every actor of one creature race is instanced from, built on
  // first sight of that race. Null (and remembered as such) when its assets are
  // missing, so those actors fall back to the human body instead of retrying.
  const Actor* CreatureTemplate(bethesda::GlobalFormId base_npc);
  bool PlayHavokClip(Actor& actor,
                     const base::String& animation_path,
                     const base::String& skeleton_hkx_path,
                     const base::String& actor_name);
  // Decodes + transcodes a clip against the actor's skeleton without touching
  // playback state (PlayHavokClip is this plus assign-and-play). Used to preload
  // the clip-cycle / additive layers. Null on a missing or unmatched file.
  std::shared_ptr<HavokClip> LoadHavokClip(const Actor& actor,
                                           const base::String& animation_path,
                                           const base::String& skeleton_hkx_path,
                                           const base::String& actor_name);
  // Samples a clip at `time` and maps its tracks into skeleton-bone space (bind
  // pose for untouched bones), through the same kinema/spline paths as the tick.
  void SampleHavokClipToPose(const Actor& actor,
                             const HavokClip& clip,
                             f32 time,
                             anim::SkeletonPose* out);
  // Transcodes a loaded clip into a kinema blob laid out in skeleton-bone order
  // (one track per skeleton bone, untouched bones at bind), so a StateMachine /
  // additive layer built over the skeleton can drive the actor pose directly.
  kinema::OwnedClip BakeSkeletonSpaceClip(const Actor& actor, const HavokClip& clip) const;
  // Builds the shared idle/walk/run locomotion machine for a character skeleton
  // from real transcoded clips (cached in character_locomotion_). Null when the
  // clips are missing/undecodable, so callers fall back to the existing path.
  std::shared_ptr<const LocomotionArchetype> BuildCharacterLocomotion(
      const Actor& actor,
      const base::String& skeleton_hkx_path,
      const base::String& actor_name);
  // Binds an actor to a locomotion archetype: sizes its instance/arena/foot-sync
  // (one-time allocation, no per-frame heap traffic).
  void AttachLocomotion(Actor& actor, std::shared_ptr<const LocomotionArchetype> arch);
  // Advances the locomotion machine one tick into actor.pose (RX_KINEMA path):
  // drives the speed/phase params, foot-syncs the gait, routes footstep events
  // and applies the machine's transition-blended root motion. dt seconds.
  void UpdateLocomotion(Actor& actor, f32 dt);
  const bethesda::HkxSkeleton* LoadHavokSkeleton(const base::String& skeleton_hkx_path);
  // Cached animationdata sidecars for an actor folder ("character", "troll").
  const ProjectAnimData* LoadProjectAnimData(const base::String& actor_name);
  // Root motion + events for one animation file, resolved through the project
  // data (clip-name match, animation-list index, then unique duration), each
  // gated on the motion duration agreeing with the decoded animation.
  void ResolveClipMotion(const ProjectAnimData& project,
                         const base::String& animation_path,
                         HavokClip* clip);
  // Lazily builds + caches the worn-armour template for a battle side (team 1
  // imperial, team 2 stormcloak), falling back to the bare body template.
  const Actor* SoldierTemplate(int team);
  bool LoadStarfieldActorTemplate(Actor* out);
  // Fallout 3 / New Vegas body: the classic skeleton + skinned upperbody/hands.
  bool LoadFalloutActorTemplate(Actor* out);
  // Decodes the FO3/NV idle/walk/run .kf clips against the actor's skeleton.
  // Null when none of them parse, leaving the procedural gait in charge.
  std::shared_ptr<const KfLocomotion> LoadFalloutLocomotion(const Actor& actor);
  void LoadBuiltinActorTemplate(Actor* out);
  bool LoadActorPart(const base::String& path, Actor& actor, i32 attach_bone = -1);
  // Attaches head-part meshes riding the head bone. With a valid `npc` it
  // assembles + morphs that NPC's FaceGen head (face/eyes/brows/beard/hair);
  // otherwise (player, soldiers) it falls back to the default male head + hair.
  // `covered_slots` are the biped slots worn armour already fills: a hood owns
  // the hair slot, so the default hairstyle underneath has to go.
  void AttachHead(Actor& actor,
                  bethesda::GlobalFormId npc,
                  bool allow_groom = true,
                  u32 covered_slots = 0);
  // Builds a strand groom from a hair nif and rides it on the head bone. Replaces
  // the flat card hair when RX_STRAND_HAIR is on. No-op if the nif has no usable
  // geometry.
  void AttachHairGroom(Actor& actor,
                       const base::String& hair_model,
                       const Vec3& tint,
                       i32 head_bone,
                       const Mat4& inverse_bind);
  bool LoadStarfieldActorPart(const base::String& path,
                              Actor& actor,
                              const bethesda::StarfieldMaterialDb& mat_db);
  base::Vector<base::String> FindHeadPartModels(u32 part_type, u32 max);
  void UpdateOneActor(Actor& actor, f32 dt);
  void EmitOneActor(Actor& actor, render::FrameView& view);

  // First-person rig internals (see the FpClip primitives in the public block).
  const HavokClip* CurrentFpClip() const;
  void AdvanceFpRig(f32 dt);                // sample the active clip into the pose
  void EmitFpRig(render::FrameView& view);  // append arm/hand + weapon draws

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
  base::UnorderedMap<base::String, base::UniquePointer<bethesda::HkxSkeleton>> havok_skeletons_;
  base::Vector<bethesda::HkxTrackPose> havok_sample_;
  base::UnorderedMap<base::String, base::UniquePointer<ProjectAnimData>> project_anim_data_;
  // Kinema sampling scratch (SoA, sized to the widest clip seen this frame).
  base::Vector<kinema::Vec3> kinema_t_;
  base::Vector<kinema::Quat> kinema_r_;
  base::Vector<f32> kinema_s_;

  // Shared idle/walk/run locomotion machine for the human character skeleton,
  // built lazily from real clips the first time an actor asks for it.
  std::shared_ptr<const LocomotionArchetype> character_locomotion_;

  base::Vector<Actor> actors_;
  i32 player_actor_ = -1;  // index into actors_ the walk mode drives, -1 = none
  bool player_seated_ = false;
  // Where a quest asked to put the player before there was a player to put, held
  // until the avatar spawns.
  base::Optional<Vec3> pending_move_;
  // Which way a resumed savegame asked the player to face, held the same way:
  // the save is read long before the body streams in.
  base::Optional<f32> pending_yaw_;
  base::Optional<Actor> npc_template_;
  base::Optional<Actor> soldier_templates_[2];  // [0] imperial (team 1), [1] stormcloak (team 2)
  // Creature rig per race (packed RACE form id); an entry with no actor is a
  // race whose rig failed to load.
  base::UnorderedMap<u64, base::Optional<Actor>> creature_templates_;
  base::UnorderedMap<u64, Actor> npc_actors_;
  base::Vector<u64> scratch_dead_actors_;
  base::UnorderedMap<u64, physics::BodyId> solid_bodies_;
  base::UniquePointer<FaceBuilder> face_builder_;  // lazily built; owns the head caches

  // First-person weapon rig: its own _1stperson skeleton actor (arms/hands +
  // a weapon riding the WEAPON node), played through the 1hm first-person clip
  // set and rooted to the camera each frame. Built lazily on the first draw.
  base::UniquePointer<FpEquipment> fp_;  // equip state machine + input
  base::Optional<Actor> fp_actor_;       // the FP arms rig (null until built)
  bool fp_ready_ = false;                // rig + clips loaded
  bool fp_engaged_ = false;              // advance the pose this frame
  bool fp_visible_ = false;              // emit draws + hide the TP body
  bool fp_clip_done_ = false;
  bool actor_dump_done_ =
      false;  // RX_ACTOR_DUMP fires once                  // active one-shot clip finished
  bool fp_clip_loop_ = true;  // idle loops; one-shots don't
  FpClip fp_current_ = FpClip::kIdle;
  f32 fp_clip_time_ = 0;
  i32 fp_weapon_bone_ = -1;                     // "WEAPON" node in the FP skeleton
  Mat4 fp_weapon_inv_bind_ = Mat4::Identity();  // inverse bind of the WEAPON node
  i32 fp_cam_bone_ = -1;                        // "Camera1st [Cam1]" node
  Vec3 fp_cam_offset_{};                        // bind Cam1 position, local metres
  bool fp_has_weapon_ = false;                  // a weapon part is attached
  Mat4 fp_view_ = Mat4::Identity();             // camera-to-world for this frame
  Mat4 fp_prev_model_ = Mat4::Identity();       // previous FP model (motion vectors)
  Mat4 fp_prev_weapon_ = Mat4::Identity();      // previous weapon transform (motion vectors)
  std::shared_ptr<const HavokClip> fp_idle_, fp_equip_, fp_unequip_, fp_attack_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_ACTOR_ACTOR_SYSTEM_H_

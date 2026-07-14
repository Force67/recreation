#include "actor_system.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <base/option.h>

#include "anim/foot_ik.h"
#include "asset/primitives.h"
#include "bethesda/converters.h"
#include "bethesda/hkx_character.h"
#include "bethesda/hkx_to_kinema.h"
#include "bethesda/material_db.h"
#include "bethesda/nif.h"
#include "bethesda/record.h"
#include "core/log.h"
#include "core/math.h"
#include "engine_internal.h"
#include "world/components.h"
#if RECREATION_HAS_NET
#include "net/replication.h"
#endif

namespace rx {

static base::Option<bool> Autowalk{"autowalk", false, "RX_AUTOWALK"};
// RX_ANIM=<internal .hkx path> selects the clip the actor bringup scene
// plays (default: a long idle fidget).
static base::Option<const char*> AnimClipPath{"anim.clip", nullptr, "RX_ANIM"};
// RX_ANIM_B=<internal .hkx path>: a second clip the bringup actor alternates
// with every 4s, inertialized across the switch (no pose pop). Kinema-only.
static base::Option<const char*> AnimClipPathB{"anim.clip_b", nullptr, "RX_ANIM_B"};
// RX_ANIM_ADDITIVE=<internal .hkx path>: an additive clip layered on top of the
// base pose each tick over its own mapped bones. Kinema-only.
static base::Option<const char*> AnimAdditivePath{"anim.additive", nullptr, "RX_ANIM_ADDITIVE"};
// RX_PIN_ROOT=1 discards a clip's root-motion translation so a locomotion clip
// (e.g. a walk cycle) marches in place instead of striding out of frame. Handy
// for demos/screenshots of the pose.
static base::Option<bool> PinRoot{"anim.pin_root", false, "RX_PIN_ROOT"};
static base::Option<bool> Player{"player", false, "RX_PLAYER"};
// Strand hair on actors: build a simulated groom from the NPC's hair nif and ride
// it on the head bone. Off falls back to the flat card the hair part uploads.
static base::Option<bool> StrandHair{"strand_hair", true, "RX_STRAND_HAIR"};
static base::Option<bool> Mq101Demo{"mq101.demo", false, "RX_MQ101_DEMO"};
// Sample Havok clips through the transcoded kinema blobs (fast path); off
// falls back to direct spline evaluation for A/B comparison.
static base::Option<bool> UseKinema{"anim.kinema", true, "RX_KINEMA"};
static base::Option<bool> Mq101Scene{"mq101.scene", false, "RX_MQ101_SCENE"};

// rec's Vec3/Quat share layout with kinema's (x,y,z / x,y,z,w), so a bone-space
// SkeletonPose can back a kinema PoseView with no copy — the inertializer and
// additive layering work directly on actor.pose's arrays.
static_assert(sizeof(Vec3) == sizeof(kinema::Vec3), "Vec3 layout mismatch");
static_assert(sizeof(Quat) == sizeof(kinema::Quat), "Quat layout mismatch");
static kinema::PoseView KinPoseView(anim::SkeletonPose& p) {
  return kinema::PoseView{reinterpret_cast<kinema::Vec3*>(p.translation.data()),
                          reinterpret_cast<kinema::Quat*>(p.rotation.data()), p.scale.data(),
                          p.size()};
}

// The shared, immutable locomotion archetype: idle/walk/run baked in
// skeleton-bone order, a 1D walk<->run blend space on the "speed" parameter and a
// two-state (idle <-> locomotion) machine with inertialized transitions. Held by
// shared_ptr and shared by every actor of the skeleton; the machine and blend
// space hold raw Clip* into the owned clips, so those keep stable addresses
// (unique_ptr, never moved after Build).
struct LocomotionArchetype {
  std::unique_ptr<kinema::OwnedClip> idle, walk, run;
  std::unique_ptr<kinema::BlendSpace> loco_space;
  kinema::StateMachine machine;
  u32 bones = 0;
  u16 idle_state = 0, loco_state = 1;
  int speed_param = 0, phase_param = 1;  // PoseParams indices
  f32 walk_speed = 1.4f, run_speed = 4.0f;  // authored planar speed (engine m/s)
  f32 walk_duration = 1.0f;                 // walk clip loop period (seconds)
};

ActorSystem::ActorSystem(EngineContext& ctx)
    : ctx_(ctx),
      world_(*ctx.world),
      renderer_(*ctx.renderer),
      physics_(*ctx.physics),
      camera_(*ctx.camera),
      config_(*ctx.config),
      vfs_(*ctx.vfs),
      records_(*ctx.records) {}

ecs::Entity ActorSystem::PlayerEntity() const {
  return player_actor_ >= 0 ? actors_[player_actor_].entity : ecs::kInvalidEntity;
}

bool ActorSystem::PlayerWorldPos(Vec3* out) const {
  if (player_actor_ < 0) return false;
  if (const world::Transform* t = world_.Get<world::Transform>(actors_[player_actor_].entity)) {
    *out = Vec3{t->position[0], t->position[1], t->position[2]};
    return true;
  }
  return false;
}

physics::CharacterId ActorSystem::PlayerCharacter() const {
  return player_actor_ >= 0 ? actors_[player_actor_].character : 0;
}

f32 ActorSystem::PlayerCapsuleOffset() const {
  return player_actor_ >= 0 ? actors_[player_actor_].capsule_offset : 0.0f;
}

f32 ActorSystem::PlayerYaw() const {
  return player_actor_ >= 0 ? actors_[player_actor_].yaw : 0.0f;
}

void ActorSystem::SetNpcGait(ecs::Entity npc, f32 speed, bool set_yaw, f32 yaw) {
  const u64 key = static_cast<u64>(npc.generation) << 32 | npc.index;
  if (Actor* a = npc_actors_.find(key)) {
    a->speed = speed;
    if (set_yaw) a->yaw = yaw;
  }
}

void ActorSystem::MovePlayer(const Vec3& velocity, bool jump, f32 yaw, bool moving, f32 speed,
                             f32 dt, Vec3* out_body) {
  if (std::getenv("RX_SKEL_DUMP")) {
    static int calls = 0;
    if (calls++ < 3) RX_INFO("MovePlayer called (vel {:.2f} {:.2f} {:.2f})", velocity.x, velocity.y, velocity.z);
  }
  if (player_actor_ < 0) return;
  Actor& actor = actors_[player_actor_];
  if (moving) actor.yaw = yaw;  // the biped's +Z faces movement
  Vec3 char_pos{};
  bool grounded = false;
  physics_.MoveCharacter(actor.character, velocity, jump, dt, &char_pos, &grounded);
  actor.speed = moving ? speed : 0.0f;
  (void)grounded;
  // Drive the entity from the capsule; the biped's body sits below the centre.
  if (world::Transform* t = world_.Get<world::Transform>(actor.entity)) {
    t->position[0] = char_pos.x;
    t->position[1] = char_pos.y - actor.capsule_offset;
    t->position[2] = char_pos.z;
    f32 h = actor.yaw * 0.5f;
    t->rotation[0] = 0;
    t->rotation[1] = std::sin(h);
    t->rotation[2] = 0;
    t->rotation[3] = std::cos(h);
  }
  *out_body = Vec3{char_pos.x, char_pos.y - actor.capsule_offset, char_pos.z};
}

void ActorSystem::CreateTestCharacter() {
  if (config_.headless) return;
  asset::Skeleton skeleton;
  asset::Mesh mesh;
  asset::MakeSkinnedBiped(asset::MakeAssetId("builtin/biped"), &skeleton, &mesh);
  renderer_.UploadMesh(mesh);

  // Ground the character stands on. Flat slab (top at y = 0.06, the sole rest
  // height) plus a raised step under the left foot so foot IK has something to
  // adapt to. Both get a static collider the IK raycast hits.
  asset::Mesh slab = asset::MakeCube(10.0f, asset::MakeAssetId("builtin/ik_ground"));
  asset::Mesh step = asset::MakeCube(0.4f, asset::MakeAssetId("builtin/ik_step"));
  renderer_.UploadMesh(slab);
  renderer_.UploadMesh(step);
  ecs::Entity ground_e = world_.Create();
  world_.Add(ground_e, world::Transform{.position = {0, -9.94f, 0}});
  world_.Add(ground_e, world::Renderable{slab.id});
  physics_.AddStaticBox({0, -9.94f, 0}, {10.0f, 10.0f, 10.0f});
  ecs::Entity step_e = world_.Create();
  world_.Add(step_e, world::Transform{.position = {0.45f, -0.15f, 0.1f}});
  world_.Add(step_e, world::Renderable{step.id});
  physics_.AddStaticBox({0.45f, -0.15f, 0.1f}, {0.4f, 0.4f, 0.4f});

  // Front view of the figure so the leg adaptation reads clearly (bringup).
  camera_.set_position({0.0f, 1.0f, 3.2f});
  camera_.set_yaw_pitch(0.0f, -0.093f);

  // Centre stage where the fly camera looks by default; the cube spins aside.
  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{.position = {0.0f, 0.0f, 0.0f}});

  Actor actor;
  actor.entity = entity;
  actor.skeleton = std::move(skeleton);
  actor.pose.ResetToBind(actor.skeleton);
  actor.speed = 0.0f;  // idle so foot IK on the uneven ground is the focus
  actor.foot_ik = true;
  actor.ik_up = {0, 1, 0};
  actor.ik_forward = {0, 0, 1};
  actor.ankle_height = 0.02f;
  // Character capsule so walk mode (T) can drive it. Capsule centre sits ~0.9m
  // above the entity origin (feet), total height ~1.7m.
  actor.capsule_offset = 0.9f;
  actor.character = physics_.CreateCharacter({0.0f, actor.capsule_offset, 0.0f}, 0.3f, 0.55f);
  ActorPart part;
  part.mesh = mesh.id;
  part.skin = mesh.skin;
  part.remap = anim::BuildBoneRemap(actor.skeleton, part.skin);
  actor.parts.push_back(std::move(part));
  anim::ComputeModelMatrices(actor.skeleton, actor.pose, &actor.bone_model);
  size_t bone_count = actor.skeleton.bones.size();
  player_actor_ = static_cast<i32>(actors_.size());
  actors_.push_back(std::move(actor));
  RX_INFO("spawned test biped ({} bones); press T to walk it", bone_count);

  if (Autowalk) {
    ctx_.auto_walk = true;
    ctx_.walk_mode = true;
  }
}

void ActorSystem::TeleportPlayer(f32 x, f32 y, f32 z) {
  // The actor bringup scene owns its stage: background start-game quests
  // (which move the player on real saves) must not yank the showcase actor
  // out of frame.
  if (config_.demo_scene == "actor") return;
  if (std::getenv("RX_SKEL_DUMP")) RX_INFO("TeleportPlayer({:.2f}, {:.2f}, {:.2f})", x, y, z);
  if (player_actor_ < 0) return;
  Actor& a = actors_[player_actor_];
  if (a.character) physics_.SetCharacterPosition(a.character, Vec3{x, y, z});
  if (world::Transform* t = world_.Get<world::Transform>(a.entity)) {
    t->position[0] = x;
    t->position[1] = y;
    t->position[2] = z;
  }
  RX_INFO("quest: teleported player to ({:.1f}, {:.1f}, {:.1f})", x, y, z);
}

bool ActorSystem::LoadActorPart(const std::string& path, Actor& actor, i32 attach_bone) {
  auto bytes = vfs_.Read(asset::NormalizePath(path));
  if (!bytes) {
    RX_WARN("actor part not found: {}", path);
    return false;
  }
  ByteSpan span(bytes->data(), bytes->size());
  asset::AssetId id = asset::MakeAssetId(path);
  bethesda::NifConversion conv = bethesda::ConvertNifSkinnedMesh(span, id, path);
  bool rigid = false;
  if (!conv.mesh || !conv.mesh->skinned || conv.mesh->skin.bones.empty()) {
    // Head/hair are static meshes rigged to a single bone, not skinned.
    if (attach_bone < 0) {
      RX_WARN("actor part {} has no skinned geometry", path);
      return false;
    }
    conv = bethesda::ConvertNifRigid(span, id, path);
    if (!conv.mesh || conv.mesh->lods.empty() || conv.mesh->lods[0].vertices.empty()) {
      RX_WARN("actor part {} has no geometry", path);
      return false;
    }
    rigid = true;
  }
  // Textures, then materials, then mesh: the cell streamer's upload order.
  for (const std::string& tex : conv.texture_paths) {
    if (const asset::Texture* t = ctx_.assets->LoadTexture(tex)) renderer_.UploadTexture(*t);
  }
  for (const asset::Material& material : conv.materials) {
    ctx_.assets->AddMaterial(material);
    renderer_.UploadMaterial(material);
  }
  renderer_.UploadMesh(*conv.mesh);

  ActorPart part;
  part.mesh = conv.mesh->id;
  if (rigid) {
    part.attach_bone = attach_bone;
    if (attach_bone < static_cast<i32>(actor.bone_model.size())) {
      part.attach_inverse_bind = Inverse(actor.bone_model[attach_bone]);
    }
    RX_INFO("actor part {}: rigid, riding bone {}", path, attach_bone);
  } else {
    part.skin = conv.mesh->skin;
    part.remap = anim::BuildBoneRemap(actor.skeleton, part.skin);
    u32 matched = 0;
    for (i32 b : part.remap) {
      if (b >= 0) ++matched;
    }
    RX_INFO("actor part {}: {} skin bones, {} matched to skeleton", path, part.skin.bones.size(),
             matched);
  }
  actor.parts.push_back(std::move(part));
  return true;
}

base::Vector<std::string> ActorSystem::FindHeadPartModels(u32 part_type, u32 max) {
  base::Vector<std::string> out;
  records_.EachOfType(FourCc('H', 'D', 'P', 'T'),
                      [&](bethesda::GlobalFormId id,
                          const bethesda::RecordStore::StoredRecord&) {
                        if (out.size() >= max) return;
                        bethesda::Record rec;
                        if (!records_.Parse(id, &rec)) return;
                        const bethesda::Subrecord* pnam = rec.Find(FourCc('P', 'N', 'A', 'M'));
                        if (!pnam || pnam->data.size() < 4) return;
                        u32 type = 0;
                        std::memcpy(&type, pnam->data.data(), 4);
                        if (type != part_type) return;
                        std::string edid = rec.GetString(FourCc('E', 'D', 'I', 'D'));
                        if (edid.find("Female") != std::string::npos ||
                            edid.find("Child") != std::string::npos) {
                          return;  // keep it a male adult head
                        }
                        std::string model = rec.GetString(FourCc('M', 'O', 'D', 'L'));
                        if (model.empty()) return;
                        std::string path = asset::NormalizePath(model);
                        if (!path.starts_with("meshes/")) path = "meshes/" + path;
                        out.push_back(path);
                      });
  return out;
}

bool ActorSystem::LoadActorTemplate(Actor* out, int soldier_kind) {
  const std::string skel_path = "meshes/actors/character/character assets/skeleton.nif";
  auto skel_bytes = vfs_.Read(asset::NormalizePath(skel_path));
  if (!skel_bytes) {
    RX_ERROR("skeleton.nif not found in the mounted archives");
    return false;
  }
  asset::Skeleton skeleton;
  if (!bethesda::ConvertNifSkeleton(ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                    asset::MakeAssetId(skel_path), &skeleton)) {
    RX_ERROR("failed to parse skeleton.nif");
    return false;
  }
  out->skeleton = std::move(skeleton);
  if (std::getenv("RX_SKEL_DUMP")) {
    for (u32 i = 0; i < out->skeleton.bones.size(); ++i) {
      const asset::Bone& bone = out->skeleton.bones[i];
      RX_INFO("nif bone [{:3}] parent {:3} t({:7.2f} {:7.2f} {:7.2f}) {}", i, bone.parent,
               bone.bind_translation.x, bone.bind_translation.y, bone.bind_translation.z,
               bone.name);
    }
  }
  out->pose.ResetToBind(out->skeleton);
  // Bethesda game space (Z-up, ~70 units/m) -> engine space (Y-up, metres).
  constexpr f32 s = 0.01428f;
  Mat4 basis{};
  basis.m[0] = s;
  basis.m[6] = -s;
  basis.m[9] = s;
  basis.m[15] = 1.0f;
  out->skeleton_to_local = basis;
  out->ik_up = {0, 0, 1};       // Bethesda up
  out->ik_forward = {0, 1, 0};  // Bethesda forward
  // Bind pose up front so rigid parts (head, hair) capture their bone's bind.
  anim::ComputeModelMatrices(out->skeleton, out->pose, &out->bone_model);

  // Body slot (slot 32): soldiers wear a faction cuirass, which both reads as a
  // soldier and, being matte armour rather than skin, stops blowing out white
  // under bright skies. The cuirass mesh carries the torso/arm/leg geometry, so
  // it replaces the bare body; hands/feet/head stay skin. Falls back to the
  // bare body if the armour mesh is missing.
  const char* body_mesh = "meshes/actors/character/character assets/malebody_1.nif";
  if (soldier_kind == 1) body_mesh = "meshes/armor/imperial/m/cuirassheavy_1.nif";
  else if (soldier_kind == 2) body_mesh = "meshes/armor/stormcloaks/cuirasssleeved_0.nif";
  bool body_ok = LoadActorPart(body_mesh, *out);
  if (!body_ok && soldier_kind != 0)
    body_ok = LoadActorPart("meshes/actors/character/character assets/malebody_1.nif", *out);
  bool any = body_ok;
  any = LoadActorPart("meshes/actors/character/character assets/malehands_1.nif", *out) || any;
  any = LoadActorPart("meshes/actors/character/character assets/malefeet_1.nif", *out) || any;
  if (!any) {
    RX_ERROR("no skyrim body parts loaded");
    return false;
  }
  // The head is attached per actor (AttachHead): NPCs get their own assembled +
  // morphed FaceGen head, the player/soldiers the default head. The shared body
  // template carries no head so it is not baked into every copy.
  return true;
}

void ActorSystem::AttachHead(Actor& actor, bethesda::GlobalFormId npc, bool allow_groom) {
  i32 head_bone = actor.skeleton.Find("NPC Head [Head]");
  if (head_bone < 0) return;
  const Mat4 inv = head_bone < static_cast<i32>(actor.bone_model.size())
                       ? Inverse(actor.bone_model[head_bone])
                       : Mat4::Identity();
  const bool groom = allow_groom && StrandHair;

  // A named NPC: assemble + morph its real FaceGen head, attach every built part
  // rigidly to the head bone. The FaceState is transient (the GPU owns the
  // uploaded meshes; the parts reference them by id), so it is dropped after.
  if (npc.plugin != 0xffff && ctx_.records) {
    if (!face_builder_) face_builder_ = std::make_unique<FaceBuilder>(ctx_);
    FaceState fs;
    if (face_builder_->AssembleNpc(npc, &fs)) {
      fs.RebuildAndUpload();
      for (const BuiltFacePart& p : fs.parts()) {
        // The flat card hair part is replaced by the simulated groom below.
        if (groom && p.type == bethesda::HeadPartType::kHair) continue;
        ActorPart part;
        part.mesh = p.mesh;
        part.attach_bone = head_bone;
        part.attach_inverse_bind = inv;
        actor.parts.push_back(std::move(part));
      }
      if (groom && !fs.hair_model().empty()) {
        const f32* hc = fs.hair_color();
        AttachHairGroom(actor, fs.hair_model(), {hc[0], hc[1], hc[2]}, head_bone, inv);
      }
      return;
    }
  }

  // Player, soldiers, or a face that failed to resolve: the default male head
  // and a hairstyle, unmorphed.
  LoadActorPart("meshes/actors/character/character assets/malehead.nif", actor, head_bone);
  base::Vector<std::string> hairs = FindHeadPartModels(/*hair=*/3, 24);
  if (groom && !hairs.empty()) {
    AttachHairGroom(actor, hairs[0], {0.32f, 0.24f, 0.18f}, head_bone, inv);
  } else {
    for (const std::string& hair : hairs)
      if (LoadActorPart(hair, actor, head_bone)) break;
  }
}

void ActorSystem::AttachHairGroom(Actor& actor, const std::string& hair_model, const Vec3& tint,
                                  i32 head_bone, const Mat4& inverse_bind) {
  if (actor.hair_groom) return;  // one groom per actor
  std::string path = asset::NormalizePath(hair_model);
  if (!path.starts_with("meshes/")) path = "meshes/" + path;
  auto bytes = vfs_.Read(path);
  if (!bytes) return;
  bethesda::NifConversion conv = bethesda::ConvertNifRigid(
      ByteSpan(bytes->data(), bytes->size()), asset::MakeAssetId(path), path);
  if (!conv.mesh || conv.mesh->lods.empty() || conv.mesh->lods[0].vertices.empty()) return;
  // Per-strand colour source: the hair nif's own diffuse.
  const asset::Texture* diffuse = nullptr;
  for (const std::string& tp : conv.texture_paths) {
    diffuse = ctx_.assets->LoadTexture(tp);
    if (diffuse) break;
  }
  render::GroomParams params;
  params.recenter = false;  // keep authored head-local coords; ride the head bone
  params.units_to_meters = 0.01428f;  // game units -> metres (matches the cell streamer)
  params.tint = tint;
  params.diffuse = diffuse;
  params.guide_count = 1600;
  params.children_per_guide = 6;
  params.strand_width = 0.0009f;
  params.clump_radius = 0.004f;
  u32 id = renderer_.CreateHairGroom(*conv.mesh, params, Mat4::Identity());
  if (!id) return;
  actor.hair_groom = id;
  actor.hair_bone = head_bone;
  actor.hair_inverse_bind = inverse_bind;
}

bool ActorSystem::LoadStarfieldActorPart(const std::string& path, Actor& actor,
                                         const bethesda::StarfieldMaterialDb& mat_db) {
  auto bytes = vfs_.Read(asset::NormalizePath(path));
  if (!bytes) {
    RX_WARN("starfield actor part not found: {}", path);
    return false;
  }
  ByteSpan span(bytes->data(), bytes->size());
  asset::AssetId id = asset::MakeAssetId(path);
  base::UniquePointer<asset::Mesh> mesh =
      bethesda::ConvertStarfieldSkinnedNif(*ctx_.assets, mat_db, span, id, path);
  if (!mesh || !mesh->skinned || mesh->skin.bones.empty()) {
    RX_WARN("starfield actor part {} has no skinned geometry", path);
    return false;
  }
  // The converter adds the material and loads its textures into the database;
  // upload both to the renderer, then the mesh, mirroring the cell streamer.
  for (const asset::MeshLod& lod : mesh->lods) {
    for (const asset::Submesh& submesh : lod.submeshes) {
      const asset::Material* material = ctx_.assets->FindMaterial(submesh.material);
      if (!material) continue;
      const asset::AssetId textures[] = {material->base_color, material->normal,
                                         material->metallic_roughness, material->emissive};
      for (asset::AssetId texture_id : textures) {
        if (!texture_id) continue;
        if (const asset::Texture* texture = ctx_.assets->FindTexture(texture_id)) {
          renderer_.UploadTexture(*texture);
        }
      }
      renderer_.UploadMaterial(*material);
    }
  }
  renderer_.UploadMesh(*mesh);

  ActorPart part;
  part.mesh = mesh->id;
  part.skin = mesh->skin;
  part.remap = anim::BuildBoneRemap(actor.skeleton, part.skin);
  u32 matched = 0;
  for (i32 b : part.remap) {
    if (b >= 0) ++matched;
  }
  RX_INFO("starfield actor part {}: {} skin bones, {} matched to skeleton", path,
           part.skin.bones.size(), matched);
  actor.parts.push_back(std::move(part));
  return true;
}

bool ActorSystem::LoadStarfieldActorTemplate(Actor* out) {
  const std::string skel_path = "meshes/actors/human/characterassets/skeleton.nif";
  auto skel_bytes = vfs_.Read(asset::NormalizePath(skel_path));
  if (!skel_bytes) {
    RX_ERROR("starfield skeleton.nif not found in the mounted archives");
    return false;
  }
  asset::Skeleton skeleton;
  if (!bethesda::ConvertNifSkeleton(ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                    asset::MakeAssetId(skel_path), &skeleton)) {
    RX_ERROR("failed to parse starfield skeleton.nif");
    return false;
  }
  out->skeleton = std::move(skeleton);
  out->pose.ResetToBind(out->skeleton);
  // Unlike Skyrim, the Starfield character assets are authored in metres, not
  // game units: the skeleton binds, the body bind transforms, and the skinned
  // ".mesh" vertices all stay in metres. So skeleton_to_local only reorients
  // Bethesda Z-up to engine Y-up (s = 1), instead of the Skyrim s = 0.01428 that
  // also converts game units to metres.
  constexpr f32 s = 1.0f;
  Mat4 basis{};
  basis.m[0] = s;
  basis.m[6] = -s;
  basis.m[9] = s;
  basis.m[15] = 1.0f;
  out->skeleton_to_local = basis;
  out->ik_up = {0, 0, 1};       // Bethesda up
  out->ik_forward = {0, 1, 0};  // Bethesda forward
  anim::ComputeModelMatrices(out->skeleton, out->pose, &out->bone_model);

  // The body materials resolve through the compiled material database, the same
  // index ConvertStarfieldNif uses for the world. Built once here for the
  // template (the bodies load only once), then dropped.
  bethesda::StarfieldMaterialDb mat_db;
  if (auto cdb = vfs_.Read("materials/materialsbeta.cdb")) {
    mat_db.Build(ByteSpan(cdb->data(), cdb->size()));
  }

  const std::string skinned_parts[] = {
      "meshes/actors/human/mesh/naked_body/naked_m.nif",
      "meshes/actors/human/mesh/nakedhands/hands_3rd_m.nif",
  };
  bool any = false;
  for (const std::string& p : skinned_parts) any = LoadStarfieldActorPart(p, *out, mat_db) || any;
  if (!any) {
    RX_ERROR("no starfield body parts loaded");
    return false;
  }
  return true;
}

void ActorSystem::LoadBuiltinActorTemplate(Actor* out) {
  // Fallback rig for games with no dedicated body loader (Fallout 4): a
  // procedural biped so their NPCs are visible placeholders rather than
  // invisible markers. Skyrim and Starfield load real skinned bodies; this
  // catches them too only when those assets fail to parse. It is already in
  // engine space (Y-up, metres), so skeleton_to_local stays identity.
  asset::Skeleton skeleton;
  asset::Mesh mesh;
  asset::MakeSkinnedBiped(asset::MakeAssetId("builtin/biped"), &skeleton, &mesh);
  renderer_.UploadMesh(mesh);
  out->skeleton = std::move(skeleton);
  out->pose.ResetToBind(out->skeleton);
  out->ik_up = {0, 1, 0};
  out->ik_forward = {0, 0, 1};
  anim::ComputeModelMatrices(out->skeleton, out->pose, &out->bone_model);
  ActorPart part;
  part.mesh = mesh.id;
  part.skin = mesh.skin;
  part.remap = anim::BuildBoneRemap(out->skeleton, part.skin);
  out->parts.push_back(std::move(part));
}

bool ActorSystem::SpawnPlayerActor(const Vec3& pos) {
  Actor actor;
  const bool starfield = ctx_.game == bethesda::Game::kStarfield;
  if (!(starfield ? LoadStarfieldActorTemplate(&actor) : LoadActorTemplate(&actor))) return false;
  // Skyrim bodies carry no head; give the player the default head + hair.
  if (ctx_.game == bethesda::Game::kSkyrimSe)
    AttachHead(actor, bethesda::GlobalFormId{0xffff, 0});

  actor.entity = world_.Create();
  world_.Add(actor.entity, world::Transform{.position = {pos.x, pos.y, pos.z}});
  actor.animate = true;
  actor.speed = 0.0f;  // idle until walk input arrives
  actor.foot_ik = true;
  // Foot IK ankle height is measured in the skeleton's own units. Skyrim's
  // skeleton is game units (~70/m), Starfield's is metres, so the same ~0.086m
  // sole-to-ankle offset is 6 there but 0.086 here.
  actor.ankle_height = starfield ? 0.086f : 6.0f;

  // Character capsule the walk mode drives. Capsule half-height+radius = 0.85,
  // so the entity origin (feet) rests at pos.y on the ground.
  actor.capsule_offset = 0.85f;
  actor.character = physics_.CreateCharacter({pos.x, pos.y + actor.capsule_offset, pos.z}, 0.3f, 0.55f);

  player_actor_ = static_cast<i32>(actors_.size());
  actors_.push_back(std::move(actor));
  return true;
}

void ActorSystem::MaybeSpawnWorldPlayer(const Vec3& ground_pos) {
  if (config_.headless) return;
  if (!config_.spawn_player && !Player && !Mq101Demo && !Mq101Scene)
    return;
  // Lift the spawn slightly so the capsule settles onto the floor instead of
  // starting embedded in the collision.
  if (!SpawnPlayerActor({ground_pos.x, ground_pos.y + 0.2f, ground_pos.z})) return;
  ctx_.walk_mode = true;
  ctx_.third_person = true;
  if (Autowalk) ctx_.auto_walk = true;
  RX_INFO("player: walkable avatar spawned at ({:.1f}, {:.1f}, {:.1f}); walk mode on", ground_pos.x,
           ground_pos.y, ground_pos.z);
}

const bethesda::HkxSkeleton* ActorSystem::LoadHavokSkeleton(
    const std::string& skeleton_hkx_path) {
  auto it = havok_skeletons_.find(skeleton_hkx_path);
  if (it != havok_skeletons_.end()) return it->second.get();
  auto bytes = vfs_.Read(asset::NormalizePath(skeleton_hkx_path));
  if (!bytes) {
    RX_ERROR("{} not found in the mounted archives", skeleton_hkx_path);
    return nullptr;
  }
  auto hkx = bethesda::HkxFile::Parse(bytes->data(), bytes->size());
  if (!hkx) {
    RX_ERROR("{} is not a supported havok packfile", skeleton_hkx_path);
    return nullptr;
  }
  bethesda::HkxPhysics physics = bethesda::DecodePhysics(*hkx);
  const bethesda::HkxSkeleton* full = nullptr;
  for (const auto& skeleton : physics.skeletons) {
    if (!full || skeleton.bones.size() > full->bones.size()) full = &skeleton;
  }
  if (!full || full->bones.empty()) {
    RX_ERROR("{} carries no skeleton", skeleton_hkx_path);
    return nullptr;
  }
  auto owned = std::make_unique<bethesda::HkxSkeleton>(*full);
  const bethesda::HkxSkeleton* result = owned.get();
  havok_skeletons_[skeleton_hkx_path] = std::move(owned);
  return result;
}

const ActorSystem::ProjectAnimData* ActorSystem::LoadProjectAnimData(
    const std::string& actor_name) {
  auto it = project_anim_data_.find(actor_name);
  if (it != project_anim_data_.end()) return it->second.get();
  auto& slot = project_anim_data_[actor_name];  // negative-cache misses too

  // The behavior project stem: the character project is "defaultmale";
  // creatures are "<name>project" (trollproject) or occasionally bare.
  base::Vector<std::string> stems;
  if (actor_name == "character") {
    stems.push_back("defaultmale");
  } else {
    stems.push_back(actor_name + "project");
    stems.push_back(actor_name);
  }
  for (const std::string& stem : stems) {
    auto project = vfs_.Read(asset::NormalizePath("meshes/animationdata/" + stem + ".txt"));
    if (!project) continue;
    auto motion =
        vfs_.Read(asset::NormalizePath("meshes/animationdata/boundanims/anims_" + stem + ".txt"));
    auto data = std::make_unique<ProjectAnimData>();
    data->data = bethesda::ParseAnimationData(
        std::string_view(reinterpret_cast<const char*>(project->data()), project->size()),
        motion ? std::string_view(reinterpret_cast<const char*>(motion->data()), motion->size())
               : std::string_view());
    // The animation list the creature clip ids index (characters/*.hkx).
    std::string chars_path = actor_name == "character"
                                 ? "meshes/actors/character/characters/defaultmale.hkx"
                                 : "meshes/actors/" + actor_name + "/characters/" + actor_name +
                                       ".hkx";
    if (auto chars = vfs_.Read(asset::NormalizePath(chars_path))) {
      if (auto hkx = bethesda::HkxFile::Parse(chars->data(), chars->size())) {
        data->animation_names = bethesda::DecodeAnimationNames(*hkx);
      }
    }
    RX_INFO("animationdata {}: {} clips, {} motion blocks, {} cached animations", stem,
             data->data.clips.size(), data->data.motion.size(), data->animation_names.size());
    slot = std::move(data);
    break;
  }
  return slot.get();
}

namespace {

std::string LowerBasename(std::string_view path) {
  size_t slash = path.find_last_of("/\\");
  if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
  size_t dot = path.rfind('.');
  if (dot != std::string_view::npos) path = path.substr(0, dot);
  std::string out(path);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

void ActorSystem::ResolveClipMotion(const ProjectAnimData& project,
                                    const std::string& animation_path, HavokClip* clip) {
  const std::string basename = LowerBasename(animation_path);
  const f32 duration = clip->animation.duration;
  // A motion block is only trusted when its duration matches the decoded
  // animation (both were authored from the same source clip); the character
  // project's clip ids drift against its animation list, so every route is
  // duration-gated.
  i32 resolved = -1;
  auto accept = [&](i32 id, const bethesda::ClipData* events_from) {
    auto it = project.data.motion.find(id);
    if (it == project.data.motion.end()) return false;
    if (std::abs(it->second.duration - duration) > 0.005f) return false;
    clip->motion = it->second;
    clip->has_motion = true;
    resolved = id;
    if (events_from) clip->events = events_from->events;
    return true;
  };
  // 1) Clip generator named like the file (the character project convention).
  for (const bethesda::ClipData& c : project.data.clips) {
    if (LowerBasename(c.name) == basename && accept(c.animation_index, &c)) return;
  }
  // 2) The file's index in the project's animation list (creature projects).
  for (size_t i = 0; i < project.animation_names.size(); ++i) {
    if (LowerBasename(project.animation_names[i]) == basename &&
        accept(static_cast<i32>(i), nullptr)) {
      break;
    }
  }
  // 3) Unique duration match across all motion blocks.
  if (!clip->has_motion) {
    i32 match = -1;
    for (const auto& [id, motion] : project.data.motion) {
      if (std::abs(motion.duration - duration) > 0.001f) continue;
      if (match >= 0) return;  // ambiguous
      match = id;
    }
    if (match >= 0) accept(match, nullptr);
  }
  // Events can still come from any clip block driving the resolved animation
  // (e.g. the troll's walk events sit on a differently-named generator).
  if (clip->has_motion && clip->events.empty()) {
    for (const bethesda::ClipData& c : project.data.clips) {
      if (c.animation_index == resolved && !c.events.empty()) {
        clip->events = c.events;
        break;
      }
    }
  }
}

std::shared_ptr<ActorSystem::HavokClip> ActorSystem::LoadHavokClip(
    const Actor& actor, const std::string& animation_path, const std::string& skeleton_hkx_path,
    const std::string& actor_name) {
  const bethesda::HkxSkeleton* havok_skeleton = LoadHavokSkeleton(skeleton_hkx_path);
  if (!havok_skeleton) return nullptr;
  auto bytes = vfs_.Read(asset::NormalizePath(animation_path));
  if (!bytes) {
    RX_ERROR("animation not found: {}", animation_path);
    return nullptr;
  }
  auto hkx = bethesda::HkxFile::Parse(bytes->data(), bytes->size());
  if (!hkx) {
    RX_ERROR("{} is not a supported havok packfile", animation_path);
    return nullptr;
  }
  auto animation = bethesda::DecodeAnimation(*hkx);
  if (!animation) {
    RX_ERROR("{} has no decodable spline-compressed animation", animation_path);
    return nullptr;
  }

  auto clip = std::make_shared<HavokClip>();
  clip->track_to_skeleton.resize(animation->num_tracks);
  u32 matched = 0;
  for (u32 t = 0; t < animation->num_tracks; ++t) {
    // The binding maps tracks to havok-skeleton bones (empty = identity);
    // names then bridge into the NIF skeleton, which shares the rig.
    u32 havok_bone = t < animation->track_to_bone.size()
                         ? static_cast<u32>(animation->track_to_bone[t])
                         : t;
    i32 bone = -1;
    if (havok_bone < havok_skeleton->bones.size()) {
      bone = actor.skeleton.Find(havok_skeleton->bones[havok_bone].name);
    }
    clip->track_to_skeleton[t] = bone;
    if (bone >= 0) ++matched;
  }
  if (matched == 0) {
    RX_INFO("havok clip {}: {} tracks, none matched the skeleton", animation_path,
             animation->num_tracks);
    return nullptr;
  }
  clip->animation = std::move(*animation);
  if (const ProjectAnimData* project = LoadProjectAnimData(actor_name)) {
    ResolveClipMotion(*project, animation_path, clip.get());
  }
  clip->kinema = kinema::OwnedClip(bethesda::TranscodeToKinema(
      clip->animation, clip->has_motion ? &clip->motion : nullptr, &clip->events));
  RX_INFO("havok clip {}: {:.2f}s, {} tracks ({} matched), motion {}, {} events, kinema {} KiB{}",
           animation_path, clip->animation.duration, clip->animation.num_tracks, matched,
           clip->has_motion ? "yes" : "no", clip->events.size(),
           clip->kinema ? clip->kinema.bytes().size() / 1024 : 0,
           clip->animation.additive ? ", ADDITIVE (played absolute)" : "");
  return clip;
}

bool ActorSystem::PlayHavokClip(Actor& actor, const std::string& animation_path,
                                const std::string& skeleton_hkx_path,
                                const std::string& actor_name) {
  auto clip = LoadHavokClip(actor, animation_path, skeleton_hkx_path, actor_name);
  if (!clip) return false;
  actor.foot_ik = false;  // the clip owns the feet
  actor.havok_time = 0;
  actor.havok_clip = std::move(clip);
  return true;
}

void ActorSystem::SampleHavokClipToPose(const Actor& actor, const HavokClip& clip, f32 time,
                                        anim::SkeletonPose* out) {
  out->ResetToBind(actor.skeleton);
  if (UseKinema && clip.kinema) {
    const u32 tracks = clip.kinema->num_tracks();
    if (kinema_t_.size() < tracks) {
      kinema_t_.resize(tracks);
      kinema_r_.resize(tracks);
      kinema_s_.resize(tracks);
    }
    kinema::PoseView pose{kinema_t_.data(), kinema_r_.data(), kinema_s_.data(), tracks};
    clip.kinema->Sample(time, pose);
    for (u32 t = 0; t < tracks && t < clip.track_to_skeleton.size(); ++t) {
      i32 bone = clip.track_to_skeleton[t];
      if (bone < 0) continue;
      out->translation[bone] = Vec3{kinema_t_[t].x, kinema_t_[t].y, kinema_t_[t].z};
      out->rotation[bone] = Quat{kinema_r_[t].x, kinema_r_[t].y, kinema_r_[t].z, kinema_r_[t].w};
      out->scale[bone] = kinema_s_[t];
    }
  } else {
    bethesda::SampleAnimation(clip.animation, time, &havok_sample_);
    for (u32 t = 0; t < havok_sample_.size() && t < clip.track_to_skeleton.size(); ++t) {
      i32 bone = clip.track_to_skeleton[t];
      if (bone < 0) continue;
      const bethesda::HkxTrackPose& sample = havok_sample_[t];
      out->translation[bone] = sample.translation;
      out->rotation[bone] =
          Quat{sample.rotation[0], sample.rotation[1], sample.rotation[2], sample.rotation[3]};
      out->scale[bone] = sample.scale;
    }
  }
}

kinema::OwnedClip ActorSystem::BakeSkeletonSpaceClip(const Actor& actor,
                                                    const HavokClip& clip) const {
  return kinema::OwnedClip(bethesda::TranscodeToKinemaSkeleton(
      clip.animation, clip.track_to_skeleton, actor.skeleton,
      clip.has_motion ? &clip.motion : nullptr, &clip.events));
}

std::shared_ptr<const LocomotionArchetype> ActorSystem::BuildCharacterLocomotion(
    const Actor& actor, const std::string& skeleton_hkx_path, const std::string& actor_name) {
  if (character_locomotion_) return character_locomotion_;

  // Try the vanilla character locomotion filenames (they vary a little across
  // releases: mtwalkforward.hkx vs mt_walkforward.hkx). First that decodes wins.
  auto load = [&](std::initializer_list<const char*> names) -> std::shared_ptr<HavokClip> {
    for (const char* n : names) {
      std::string path = std::string("meshes/actors/character/animations/") + n;
      if (!vfs_.Contains(asset::NormalizePath(path))) continue;
      if (auto c = LoadHavokClip(actor, path, skeleton_hkx_path, actor_name)) return c;
    }
    return nullptr;
  };
  auto idle = load({"male/mt_idle.hkx", "mt_idle.hkx", "mtidle.hkx"});
  auto walk = load({"male/mt_walkforward.hkx", "mt_walkforward.hkx", "mtwalkforward.hkx"});
  auto run = load({"male/mt_runforward.hkx", "mt_runforward.hkx", "mtrunforward.hkx"});
  if (!idle || !walk || !run) {
    RX_WARN("locomotion machine: missing clips (idle {} walk {} run {}); actor stays on the "
            "direct-clip/procedural path",
            static_cast<bool>(idle), static_cast<bool>(walk), static_cast<bool>(run));
    return nullptr;
  }

  auto arch = std::make_shared<LocomotionArchetype>();
  arch->bones = static_cast<u32>(actor.skeleton.bones.size());
  arch->idle = std::make_unique<kinema::OwnedClip>(BakeSkeletonSpaceClip(actor, *idle));
  arch->walk = std::make_unique<kinema::OwnedClip>(BakeSkeletonSpaceClip(actor, *walk));
  arch->run = std::make_unique<kinema::OwnedClip>(BakeSkeletonSpaceClip(actor, *run));
  if (!*arch->idle || !*arch->walk || !*arch->run) {
    RX_WARN("locomotion machine: a clip failed to transcode; skipping");
    return nullptr;
  }

  // Authored planar speed of a gait clip = its total root travel / duration,
  // mapped from Bethesda game units (Z-up, ~70/m) to engine metres.
  auto planar_speed = [](const HavokClip& c, f32 fallback) -> f32 {
    if (!c.has_motion || c.animation.duration < 1e-3f) return fallback;
    Vec3 d = bethesda::SampleMotionTranslation(c.motion, c.animation.duration);
    constexpr f32 s = 0.01428f;
    f32 metres = std::sqrt(d.x * d.x + d.y * d.y) * s;
    f32 sp = metres / c.animation.duration;
    return sp > 0.05f ? sp : fallback;
  };
  arch->walk_speed = planar_speed(*walk, 1.4f);
  arch->run_speed = planar_speed(*run, 4.0f);
  if (arch->run_speed <= arch->walk_speed + 0.1f) arch->run_speed = arch->walk_speed + 1.5f;
  arch->walk_duration = std::max(arch->walk->get()->duration(), 1.0f / 30.0f);

  // 1D walk<->run blend space on the "speed" parameter.
  arch->loco_space = std::make_unique<kinema::BlendSpace>(kinema::BlendSpace::Dim::k1D);
  arch->loco_space->Add(arch->walk->get(), arch->walk_speed);
  arch->loco_space->Add(arch->run->get(), arch->run_speed);
  arch->loco_space->Finalize();

  // idle <-> locomotion, transitions gated on "speed". The locomotion state is a
  // one-op blend-space program whose normalized phase (phase_param) the tick
  // drives from a foot-sync group; walk supplies events + root motion.
  kinema::StateMachineBuilder smb(arch->bones);
  arch->idle_state = smb.AddClipState(arch->idle->get(), /*loop=*/true);
  kinema::PoseOp loco{};
  loco.kind = kinema::PoseOp::Kind::kBlendSpace;
  loco.dst = 0;
  loco.a = 1;  // scratch register
  loco.space = arch->loco_space.get();
  loco.coord_param = static_cast<kinema::i16>(arch->speed_param);
  loco.time_param = static_cast<kinema::i16>(arch->phase_param);
  arch->loco_state = smb.AddProgramState(&loco, 1, /*reg_count=*/2, /*clock_op=*/-1,
                                         /*loop_duration=*/arch->walk_duration, /*loop=*/true,
                                         /*speed=*/1.0f, /*clip=*/arch->walk->get(),
                                         /*root_source=*/arch->walk->get());
  kinema::TransitionDesc start;
  start.duration = 0.22f;
  kinema::ConditionAtom moving =
      kinema::ConditionAtom::Greater(static_cast<kinema::i16>(arch->speed_param), 0.5f);
  smb.AddTransition(arch->idle_state, arch->loco_state, start, &moving, 1);
  kinema::TransitionDesc stop;
  stop.duration = 0.28f;
  kinema::ConditionAtom halting =
      kinema::ConditionAtom::Less(static_cast<kinema::i16>(arch->speed_param), 0.35f);
  smb.AddTransition(arch->loco_state, arch->idle_state, stop, &halting, 1);
  arch->machine = smb.Build();

  RX_INFO("locomotion machine: idle/walk/run baked ({} bones), walk {:.2f} m/s, run {:.2f} m/s",
          arch->bones, arch->walk_speed, arch->run_speed);
  character_locomotion_ = arch;
  return arch;
}

void ActorSystem::AttachLocomotion(Actor& actor, std::shared_ptr<const LocomotionArchetype> arch) {
  if (!arch) return;
  actor.loco_arch = arch;
  actor.loco_params.assign(2, 0.0f);
  actor.loco_arena.Init(arch->bones, arch->machine.max_registers());
  actor.loco_sm.Init(arch->machine, arch->idle_state);
  // Foot-sync walk<->run only when both clips carry the same non-empty footfall
  // marker set (their sidecar events); otherwise a plain phase clock drives it.
  const kinema::Clip* w = arch->walk->get();
  const kinema::Clip* r = arch->run->get();
  actor.loco_synced = w && r && w->num_events() > 0 && w->num_events() == r->num_events();
  actor.loco_sync.Clear();
  if (actor.loco_synced) {
    actor.loco_sync.AddClip(*w);
    actor.loco_sync.AddClip(*r);
    actor.loco_sync.Reset(0.0f);
  }
  actor.loco_prev_phase = 0.0f;
  actor.loco_phase = 0.0f;
  actor.foot_ik = false;  // the machine owns the pose
}

void ActorSystem::UpdateLocomotion(Actor& actor, f32 dt) {
  const LocomotionArchetype& arch = *actor.loco_arch;

  // RX_ANIM_B debug driver: script the speed parameter idle -> walk -> run so the
  // otherwise-static bringup scene exercises the machine's inertialized
  // transitions and its walk<->run blend space. The inertializer absorbs the
  // stepped coordinate change into a smooth pose blend.
  f32 speed = actor.speed;
  if (actor.loco_debug_drive) {
    actor.loco_debug_t += dt;
    const f32 seg = 3.0f;  // seconds per gait phase
    f32 cycle = std::fmod(actor.loco_debug_t, seg * 3.0f);
    speed = cycle < seg ? 0.0f : (cycle < 2.0f * seg ? arch.walk_speed : arch.run_speed);
  }
  actor.loco_params[arch.speed_param] = speed;

  // Shared normalized locomotion phase [0,1): foot-synced when the gaits share
  // markers, else a plain clock whose cadence scales with speed.
  f32 phase;
  if (actor.loco_synced) {
    f32 play_rate = (speed > 0.05f ? speed : 0.0f) / std::max(arch.walk_speed, 0.05f);
    actor.loco_sync.Advance(dt, /*leader=*/0, play_rate);
    u32 markers = actor.loco_sync.marker_count();
    phase = markers > 0 ? actor.loco_sync.phase() / static_cast<f32>(markers) : 0.0f;
  } else {
    f32 cadence = (speed > 0.05f ? speed : arch.walk_speed) / std::max(arch.walk_speed, 0.05f);
    actor.loco_phase += dt * cadence / arch.walk_duration;
    actor.loco_phase -= std::floor(actor.loco_phase);
    phase = actor.loco_phase;
  }
  actor.loco_params[arch.phase_param] = phase;

  kinema::PoseParams pp{actor.loco_params.data(), static_cast<u32>(actor.loco_params.size())};
  kinema::PoseView out = KinPoseView(actor.pose);

  // Route the active state's footstep point events (and any ranged contact spans)
  // to the log through the machine's event sink.
  kinema::EventCallback ev{};
  ev.point = [](void*, const kinema::ClipEvent& e) {
    RX_DEBUG("anim event '{}' @ {:.2f}s", e.name ? e.name : "?", e.time);
  };
  ev.ranged = [](void*, const kinema::ClipRangedEvent& r, kinema::RangePhase p) {
    const char* ph = p == kinema::RangePhase::kEnter    ? "enter"
                     : p == kinema::RangePhase::kActive ? "active"
                                                        : "exit";
    RX_DEBUG("anim range '{}' {} [{:.2f},{:.2f}]", r.name ? r.name : "?", ph, r.begin, r.end);
  };
  const u16 prev_state = actor.loco_sm.state();
  actor.loco_sm.Update(dt, pp, actor.loco_arena, out, &ev);
  if (actor.loco_sm.state() != prev_state) {
    RX_INFO("locomotion transition: state {} -> {} (speed {:.2f} m/s)", prev_state,
            actor.loco_sm.state(), speed);
  }
  actor.loco_prev_phase = phase;

  // Root motion: the machine's transition-blended, loop-aware delta (game units,
  // Z-up) into engine space (Y-up, metres) and the actor's facing — the same
  // basis the direct-clip path uses. Only for showcase actors; capsule-driven
  // gameplay actors keep their controller-owned position.
  if (actor.loco_apply_root && !PinRoot) {
    kinema::Vec3 kd = actor.loco_sm.RootMotion();
    constexpr f32 s = 0.01428f;
    Vec3 local{kd.x * s, kd.z * s, -kd.y * s};
    f32 cy = std::cos(actor.yaw), sy = std::sin(actor.yaw);
    Vec3 step{local.x * cy + local.z * sy, local.y, -local.x * sy + local.z * cy};
    if (world::Transform* t = world_.Get<world::Transform>(actor.entity)) {
      t->position[0] += step.x;
      t->position[1] += step.y;
      t->position[2] += step.z;
    }
  }
}

// Spawns a creature rig (its own skeleton.nif + skinned mesh + skeleton.hkx)
// on the bringup stage and plays a clip from its animation set.
bool ActorSystem::CreateCreatureActor(const std::string& name, const std::string& clip_override) {
  const std::string base = "meshes/actors/" + name + "/";
  const std::string skel_path = base + "character assets/skeleton.nif";
  auto skel_bytes = vfs_.Read(asset::NormalizePath(skel_path));
  if (!skel_bytes) {
    RX_ERROR("{} not found in the mounted archives", skel_path);
    return false;
  }
  Actor actor;
  if (!bethesda::ConvertNifSkeleton(ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                    asset::MakeAssetId(skel_path), &actor.skeleton)) {
    RX_ERROR("failed to parse {}", skel_path);
    return false;
  }
  actor.pose.ResetToBind(actor.skeleton);
  constexpr f32 s = 0.01428f;  // Bethesda Z-up game units -> engine Y-up metres
  Mat4 basis{};
  basis.m[0] = s;
  basis.m[6] = -s;
  basis.m[9] = s;
  basis.m[15] = 1.0f;
  actor.skeleton_to_local = basis;
  anim::ComputeModelMatrices(actor.skeleton, actor.pose, &actor.bone_model);
  if (!LoadActorPart(base + "character assets/" + name + ".nif", actor)) return false;

  actor.entity = world_.Create();
  world_.Add(actor.entity, world::Transform{.position = {0, 0, 0}});
  actor.animate = true;
  actor.foot_ik = false;

  // Clip: explicit override, else the creature's common locomotion/idle names.
  base::Vector<std::string> candidates;
  if (!clip_override.empty()) candidates.push_back(clip_override);
  candidates.push_back(base + "animations/mtwalkforward.hkx");
  candidates.push_back(base + "animations/mt_walkforward.hkx");
  candidates.push_back(base + "animations/mt_idle.hkx");
  candidates.push_back(base + "animations/h2hidle.hkx");
  candidates.push_back(base + "animations/idle_pet.hkx");
  bool playing = false;
  for (const std::string& clip : candidates) {
    if (!vfs_.Contains(asset::NormalizePath(clip))) continue;
    if (PlayHavokClip(actor, clip, base + "character assets/skeleton.hkx", name)) {
      playing = true;
      break;
    }
  }
  if (!playing) RX_WARN("{}: no playable clip found, holding bind pose", name);

  RX_INFO("creature actor '{}': {} bones, {} parts", name, actor.skeleton.bones.size(),
           actor.parts.size());
  actors_.push_back(std::move(actor));
  return true;
}

bool ActorSystem::CreateSkyrimActor() {
  // RX_CREATURE=<troll|horse|...> swaps the human for a creature rig.
  if (const char* creature = std::getenv("RX_CREATURE")) {
    const char* clip = AnimClipPath.get();
    if (!CreateCreatureActor(creature, clip ? clip : "")) return false;
    asset::Mesh slab = asset::MakeCube(10.0f, asset::MakeAssetId("builtin/actor_ground"));
    renderer_.UploadMesh(slab);
    ecs::Entity ground = world_.Create();
    world_.Add(ground, world::Transform{.position = {0, -10.0f, 0}});
    world_.Add(ground, world::Renderable{slab.id});
    if (physics_.initialized()) physics_.AddStaticBox({0, -10.0f, 0}, {10, 10, 10});
    // Creatures are bigger than people; frame from further back.
    camera_.set_position({0.0f, 1.4f, -4.5f});
    camera_.set_yaw_pitch(3.14159f, -0.08f);
    return true;
  }
  if (!SpawnPlayerActor({0, 0, 0})) return false;
  RX_INFO("loaded skyrim skeleton: {} bones", actors_[player_actor_].skeleton.bones.size());

  // A ground slab to stand on (top at y = 0, where the skeleton's feet sit),
  // plus a step under one foot so the foot IK has something to adapt to.
  asset::Mesh slab = asset::MakeCube(10.0f, asset::MakeAssetId("builtin/actor_ground"));
  asset::Mesh step = asset::MakeCube(0.45f, asset::MakeAssetId("builtin/actor_step"));
  renderer_.UploadMesh(slab);
  renderer_.UploadMesh(step);
  ecs::Entity ground = world_.Create();
  world_.Add(ground, world::Transform{.position = {0, -10.0f, 0}});
  world_.Add(ground, world::Renderable{slab.id});
  // Clear of the spawn capsule (radius 0.3): with real physics the character
  // controller depenetrates from anything it overlaps and the actor drifts
  // out of frame.
  ecs::Entity step_e = world_.Create();
  world_.Add(step_e, world::Transform{.position = {0.9f, -0.33f, 0}});
  world_.Add(step_e, world::Renderable{step.id});
  if (physics_.initialized()) {
    physics_.AddStaticBox({0, -10.0f, 0}, {10, 10, 10});
    physics_.AddStaticBox({0.9f, -0.33f, 0}, {0.45f, 0.45f, 0.45f});
  }

  // The skeleton faces -Z in engine space, so frame it from the front.
  camera_.set_position({0.0f, 0.95f, -3.0f});
  camera_.set_yaw_pitch(3.14159f, -0.08f);
  // Real Havok animation on the bringup actor; RX_ANIM overrides the clip,
  // and a failed load falls back to the procedural gait.
  const char* clip_override = AnimClipPath.get();
  const bool explicit_clip = clip_override && clip_override[0];
  std::string clip_path = explicit_clip
                              ? clip_override
                              : "meshes/actors/character/animations/mt_idle_b_long_left.hkx";
  const std::string skel_hkx = "meshes/actors/character/character assets/skeleton.hkx";
  // The idle clip stays resident as the RX_KINEMA=0 fallback pose; under
  // RX_KINEMA the locomotion machine below takes precedence.
  PlayHavokClip(actors_[player_actor_], clip_path, skel_hkx, "character");

  // Locomotion state machine: unless RX_ANIM pins one explicit clip, bind the
  // bringup actor to the shared idle/walk/run machine so RX_KINEMA drives the
  // pose through kinema's state layer. RX_ANIM_B scripts it idle->walk->run.
  if (!explicit_clip) {
    Actor& a = actors_[player_actor_];
    if (auto arch = BuildCharacterLocomotion(a, skel_hkx, "character")) {
      AttachLocomotion(a, arch);
      a.loco_apply_root = true;  // showcase actor: the machine may move it
      if (const char* b = AnimClipPathB.get(); b && b[0]) {
        a.loco_debug_drive = true;
        RX_INFO("anim RX_ANIM_B: driving the locomotion machine idle->walk->run (inertialized)");
      }
    }
  }
  // Additive layer: bake the clip into a kinema additive (delta) clip in
  // skeleton space (first-frame reference) and compose it each tick with
  // kinema::ApplyAdditive (RX_KINEMA path). Untouched bones carry an identity
  // delta, so the single-skeleton additive covers name-mapped bones cleanly.
  if (const char* add = AnimAdditivePath.get(); add && add[0]) {
    Actor& a = actors_[player_actor_];
    if (auto layer = LoadHavokClip(a, add, skel_hkx, "character")) {
      kinema::OwnedClip baked_src = BakeSkeletonSpaceClip(a, *layer);
      if (baked_src) {
        a.additive_clip = layer;
        a.additive_baked = std::make_shared<kinema::OwnedClip>(
            kinema::MakeAdditiveClipFromFirstFrame(*baked_src.get()));
        a.additive_time = 0;
        RX_INFO("anim additive layer baked (skeleton-space, first-frame reference): {}", add);
      }
    }
  }
  RX_INFO("skyrim actor ready ({} body parts); press T to walk it",
           actors_[player_actor_].parts.size());
  if (Autowalk) {
    ctx_.auto_walk = true;
    ctx_.walk_mode = true;
  }
  return true;
}

void ActorSystem::Update(f32 dt) {
  for (Actor& actor : actors_) UpdateOneActor(actor, dt);
  for (auto entry : npc_actors_) UpdateOneActor(entry.value, dt);
}

void ActorSystem::UpdateOneActor(Actor& actor, f32 dt) {
  if (!actor.animate) {
    actor.pose.ResetToBind(actor.skeleton);
    anim::ComputeModelMatrices(actor.skeleton, actor.pose, &actor.bone_model);
    return;
  }
#if RECREATION_HAS_NET
  // A replicated actor (multiplayer client) gets no local steering, so drive its
  // gait from the speed the net layer derived from its interpolated motion;
  // otherwise it would slide without stepping.
  if (const net::ReplicatedGait* gait = world_.Get<net::ReplicatedGait>(actor.entity))
    actor.speed = gait->speed;
#endif
  const bool use_kinema = UseKinema;
  if (use_kinema && actor.loco_arch) {
    // RX_KINEMA locomotion: the compiled idle/walk/run machine drives the pose,
    // its transitions, blend space, foot-sync, events and root motion (item 1-4).
    UpdateLocomotion(actor, dt);
  } else if (actor.havok_clip) {
    // Direct one-shot / idle clip path (attacks, gameplay-triggered idles, and
    // the RX_KINEMA=0 spline fallback). Unchanged playback.
    const bool kin = UseKinema && actor.havok_clip->kinema;
    const HavokClip& clip = *actor.havok_clip;
    f32 duration = std::max(clip.animation.duration, 1.0f / 30.0f);
    f32 prev_time = actor.havok_time;
    actor.havok_time = std::fmod(actor.havok_time + dt, duration);
    bool wrapped = actor.havok_time < prev_time;
    // Root motion: the sidecar's cumulative offsets become a per-tick world
    // delta, through the same Z-up-game-units -> Y-up-metres basis the mesh
    // uses, then the actor's facing.
    if (clip.has_motion && !PinRoot) {
      Vec3 d;
      if (kin) {
        kinema::Vec3 kd = clip.kinema->RootDelta(prev_time, actor.havok_time);
        d = Vec3{kd.x, kd.y, kd.z};
      } else {
        d = bethesda::MotionTranslationDelta(clip.motion, prev_time, actor.havok_time);
      }
      constexpr f32 s = 0.01428f;
      Vec3 local{d.x * s, d.z * s, -d.y * s};
      f32 cy = std::cos(actor.yaw), sy = std::sin(actor.yaw);
      Vec3 step{local.x * cy + local.z * sy, local.y, -local.x * sy + local.z * cy};
      if (world::Transform* t = world_.Get<world::Transform>(actor.entity)) {
        t->position[0] += step.x;
        t->position[1] += step.y;
        t->position[2] += step.z;
      }
    }
    // Trigger events (footsteps, sync markers) whose time passed this tick.
    if (kin) {
      clip.kinema->EventsInRange(prev_time, actor.havok_time, [&](const kinema::ClipEvent& e) {
        RX_DEBUG("anim event '{}' @ {:.2f}s", e.name, e.time);
      });
    } else {
      for (const bethesda::ClipEvent& event : clip.events) {
        bool fired = wrapped ? (event.time > prev_time || event.time <= actor.havok_time)
                             : (event.time > prev_time && event.time <= actor.havok_time);
        if (fired) RX_DEBUG("anim event '{}' @ {:.2f}s", event.name, event.time);
      }
    }
    SampleHavokClipToPose(actor, clip, actor.havok_time, &actor.pose);
  } else {
    actor.locomotion.phase = anim::AdvancePhase(actor.locomotion.phase, actor.speed, dt);
    actor.locomotion.Apply(actor.skeleton, actor.speed, &actor.pose);
  }

  // Additive layer (RX_KINEMA path): compose the baked additive (delta) clip onto
  // the pose the base state produced, over the whole skeleton — untouched bones
  // carry an identity delta so ApplyAdditive leaves them unchanged (item 5).
  if (use_kinema && actor.additive_baked && *actor.additive_baked) {
    const kinema::Clip* add = actor.additive_baked->get();
    f32 add_dur = std::max(add->duration(), 1.0f / 30.0f);
    actor.additive_time = std::fmod(actor.additive_time + dt, add_dur);
    const u32 n = actor.pose.size();
    if (kinema_t_.size() < n) {
      kinema_t_.resize(n);
      kinema_r_.resize(n);
      kinema_s_.resize(n);
    }
    kinema::PoseView layer{kinema_t_.data(), kinema_r_.data(), kinema_s_.data(), n};
    add->Sample(actor.additive_time, layer);
    kinema::PoseView base = KinPoseView(actor.pose);
    kinema::ApplyAdditive(base, layer, 1.0f, base);
  }

  if (actor.foot_ik && physics_.initialized()) {
    const world::Transform* t = world_.Get<world::Transform>(actor.entity);
    Mat4 model = (t ? TransformMatrix(*t) : Mat4::Identity()) * actor.skeleton_to_local;
    Mat4 inv_model = Inverse(model);
    // Foot IK works in model space; bridge to the engine-space physics world.
    auto ground = [&](const Vec3& origin, Vec3* hit, Vec3* normal) -> bool {
      physics::PhysicsWorld::RayHit rh;
      if (!physics_.Raycast(TransformPoint(model, origin), {0, -1, 0}, 3.0f, &rh)) return false;
      *hit = TransformPoint(inv_model, rh.position);
      *normal = Normalize(TransformDir(inv_model, rh.normal));
      return true;
    };
    // Stance weights: at speed the planted foot gets IK while the swing foot is
    // left to lift; when nearly idle both feet plant.
    f32 theta = actor.locomotion.phase * 6.2831853f;
    f32 leg = std::sin(theta);
    f32 idle = std::clamp(1.0f - actor.speed, 0.0f, 1.0f);
    f32 foot_weight[2] = {std::max(idle, std::clamp(0.5f + leg * 2.0f, 0.0f, 1.0f)),
                          std::max(idle, std::clamp(0.5f - leg * 2.0f, 0.0f, 1.0f))};
    anim::SolveFootIk(actor.skeleton, ground, actor.ik_up, actor.ik_forward, actor.ankle_height,
                      foot_weight, &actor.pose, &actor.bone_model);
  } else {
    anim::ComputeModelMatrices(actor.skeleton, actor.pose, &actor.bone_model);
  }
}

void ActorSystem::EmitDraws(render::FrameView& view) {
  for (Actor& actor : actors_) EmitOneActor(actor, view);
  for (auto entry : npc_actors_) EmitOneActor(entry.value, view);
}

void ActorSystem::EmitOneActor(Actor& actor, render::FrameView& view) {
  const world::Transform* t = world_.Get<world::Transform>(actor.entity);
  Mat4 model = (t ? TransformMatrix(*t) : Mat4::Identity()) * actor.skeleton_to_local;
  // Colour battle actors by their combat side so the armies read apart on the
  // field (a generic engine cue, not quest-specific): team 1 warm red, team 2
  // cool blue. Actors with no team (the player, town NPCs) stay untinted.
  u32 tint = 0;
  if (const world::CombatTeam* ct = world_.Get<world::CombatTeam>(actor.entity)) {
    if (ct->team == 1) tint = 0xC85040u;       // imperial-side red
    else if (ct->team == 2) tint = 0x4078C8u;  // stormcloak-side blue
  }
  if (std::getenv("RX_SKEL_DUMP") && actor.parts.size() > 2) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RX_INFO("actor emit: model t=({:.2f} {:.2f} {:.2f}) scale col0=({:.4f} {:.4f} {:.4f})",
               model.m[12], model.m[13], model.m[14], model.m[0], model.m[1], model.m[2]);
      i32 com = actor.skeleton.Find("NPC COM [COM ]");
      if (com >= 0 && com < static_cast<i32>(actor.bone_model.size())) {
        const Mat4& b = actor.bone_model[com];
        RX_INFO("actor emit: bone_model[COM] t=({:.2f} {:.2f} {:.2f})", b.m[12], b.m[13],
                 b.m[14]);
      }
      RX_INFO("actor emit: {} parts, first mesh hash {:x}", actor.parts.size(),
               actor.parts[0].mesh.hash);
    }
  }
  for (ActorPart& part : actor.parts) {
    render::DrawItem item;
    item.mesh = part.mesh.hash;
    item.tint = tint;
    if (part.attach_bone >= 0 && part.attach_bone < static_cast<i32>(actor.bone_model.size())) {
      // Rigid part: ride the bone's animated delta from its bind transform.
      item.transform = model * actor.bone_model[part.attach_bone] * part.attach_inverse_bind;
      item.prev_transform = item.transform;
      item.skin_offset = -1;
    } else {
      item.transform = model;
      item.prev_transform = actor.prev_model;
      item.skin_offset = static_cast<i32>(view.bone_matrices.size());
      base::Vector<Mat4> palette;
      anim::BuildSkinPalette(actor.bone_model, part.skin, part.remap, &palette);
      for (const Mat4& m : palette) view.bone_matrices.push_back(m);
    }
    view.draws.push_back(item);
  }
  // Ride the strand groom on the head bone. The groom keeps its authored, engine-
  // scaled head-local coordinates (built with recenter off), so the head part's
  // own transform, with the skeleton->local scale peeled back off, places it.
  if (actor.hair_groom && actor.hair_bone >= 0 &&
      actor.hair_bone < static_cast<i32>(actor.bone_model.size())) {
    Mat4 head = model * actor.bone_model[actor.hair_bone] * actor.hair_inverse_bind;
    renderer_.SetHairGroomTransform(actor.hair_groom, head * Inverse(actor.skeleton_to_local));
  }
  actor.prev_model = model;
}

const ActorSystem::Actor* ActorSystem::SoldierTemplate(int team) {
  if (team != 1 && team != 2) return nullptr;
  std::optional<Actor>& slot = soldier_templates_[team - 1];
  if (!slot) {
    Actor tmpl;
    if (!LoadActorTemplate(&tmpl, team)) return nullptr;  // fall back to the bare body
    // No groom on a shared template: the copies would alias one GPU groom. Battle
    // soldiers keep the flat card hair.
    AttachHead(tmpl, bethesda::GlobalFormId{0xffff, 0}, /*allow_groom=*/false);
    tmpl.animate = true;
    tmpl.speed = 0.0f;
    tmpl.foot_ik = false;
    slot = std::move(tmpl);
    RX_INFO("soldier template ready: team {} ({} parts)", team, slot->parts.size());
  }
  return &*slot;
}

void ActorSystem::SyncNpcActors() {
  if (config_.headless) return;  // dedicated server doesn't render NPCs
  // Build the shared rig once; every NPC actor is instanced from it.
  if (!npc_template_) {
    Actor tmpl;
    bool loaded = ctx_.game == bethesda::Game::kStarfield ? LoadStarfieldActorTemplate(&tmpl)
                                                          : LoadActorTemplate(&tmpl);
    if (!loaded) {
      // The game's skinned body assets are absent or did not parse (e.g. a
      // Fallout 4 session): fall back to the builtin biped so NPCs still
      // populate the world.
      tmpl = Actor{};
      LoadBuiltinActorTemplate(&tmpl);
      RX_INFO("npc rendering: using the builtin biped (game body assets absent)");
    }
    tmpl.animate = true;
    tmpl.speed = 0.0f;     // idle
    tmpl.foot_ik = false;  // skip per-NPC ground raycasts
    npc_template_ = std::move(tmpl);
    RX_INFO("npc actor template ready ({} parts)", npc_template_->parts.size());
  }
  // Give every NPC entity without one a skinned actor instance (own pose, GPU
  // meshes shared by hash with the template). Battle actors (those on a combat
  // team) instance from their faction's armoured template instead of the bare
  // civilian body, so the two armies read as soldiers.
  world_.Each<world::Npc, world::Transform>([&](ecs::Entity e, world::Npc& npc, world::Transform&) {
    if (world_.Has<world::Hidden>(e) || world_.Has<world::Deleted>(e)) return;
    const u64 key = static_cast<u64>(e.generation) << 32 | e.index;
    if (npc_actors_.find(key)) return;
    const Actor* tmpl = &*npc_template_;
    bool is_soldier = false;
    if (const world::CombatTeam* ct = world_.Get<world::CombatTeam>(e))
      if (const Actor* st = SoldierTemplate(ct->team)) {
        tmpl = st;
        is_soldier = true;
      }
    Actor a = *tmpl;
    a.entity = e;
    a.character = 0;
    a.pose.ResetToBind(a.skeleton);
    // Skyrim civilians get their own assembled, morphed FaceGen head; soldiers
    // already carry the default head from their (armoured) template copy.
    if (!is_soldier && ctx_.game == bethesda::Game::kSkyrimSe) AttachHead(a, npc.base);
    npc_actors_.insert(key, std::move(a));
  });
  // Drop actors whose NPC entity streamed out, so none render at the origin.
  scratch_dead_actors_.clear();
  for (auto entry : npc_actors_)
    if (!world_.IsAlive(entry.value.entity) || world_.Has<world::Hidden>(entry.value.entity) ||
        world_.Has<world::Deleted>(entry.value.entity))
      scratch_dead_actors_.push_back(entry.key);
  for (u64 key : scratch_dead_actors_) {
    if (Actor* a = npc_actors_.find(key); a && a->hair_groom) renderer_.DestroyHairGroom(a->hair_groom);
    npc_actors_.erase(key);
  }
}

void ActorSystem::SyncSolidBodies() {
  if (config_.headless || !physics_.initialized()) return;
  constexpr f32 kRadius = 0.3f, kHalfHeight = 0.55f;
  constexpr f32 kCentreOffset = kRadius + kHalfHeight;  // feet -> capsule centre
  [[maybe_unused]] const ecs::Entity local =
      player_actor_ >= 0 ? actors_[player_actor_].entity : ecs::kInvalidEntity;

  auto ensure = [&](ecs::Entity e, const world::Transform& t) {
    const u64 key = static_cast<u64>(e.generation) << 32 | e.index;
    const Vec3 centre{t.position[0], t.position[1] + kCentreOffset, t.position[2]};
    if (physics::BodyId* body = solid_bodies_.find(key)) {
      physics_.SetBodyPosition(*body, centre, t.rotation);
    } else if (physics::BodyId id = physics_.AddKinematicCapsule(centre, kRadius, kHalfHeight)) {
      solid_bodies_.insert(key, id);
    }
  };

  world_.Each<world::Npc, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::Transform& t) {
        if (world_.Has<world::Hidden>(e) || world_.Has<world::Deleted>(e)) return;
        ensure(e, t);
      });
  // Other (networked) players are solid too; never block the local player itself.
#if RECREATION_HAS_NET
  world_.Each<net::NetworkId, world::Transform>(
      [&](ecs::Entity e, net::NetworkId&, world::Transform& t) {
        if (e.index == local.index && e.generation == local.generation) return;
        ensure(e, t);
      });
#endif

  // Drop capsules whose entity is gone (cell unload / player disconnect).
  scratch_dead_actors_.clear();
  for (auto entry : solid_bodies_) {
    const ecs::Entity e{static_cast<u32>(entry.key & 0xffffffffu), static_cast<u32>(entry.key >> 32)};
    if (!world_.IsAlive(e) || world_.Has<world::Hidden>(e) || world_.Has<world::Deleted>(e))
      scratch_dead_actors_.push_back(entry.key);
  }
  for (u64 key : scratch_dead_actors_) {
    if (physics::BodyId* body = solid_bodies_.find(key)) physics_.RemoveBody(*body);
    solid_bodies_.erase(key);
  }
}

}  // namespace rx

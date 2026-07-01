#include "actor_system.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <base/option.h>

#include "anim/foot_ik.h"
#include "asset/primitives.h"
#include "bethesda/converters.h"
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

namespace rec {

static base::Option<bool> Autowalk{"autowalk", false, "REC_AUTOWALK"};
static base::Option<bool> Player{"player", false, "REC_PLAYER"};
static base::Option<bool> Mq101Demo{"mq101.demo", false, "REC_MQ101_DEMO"};
static base::Option<bool> Mq101Scene{"mq101.scene", false, "REC_MQ101_SCENE"};

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
  REC_INFO("spawned test biped ({} bones); press T to walk it", bone_count);

  if (Autowalk) {
    ctx_.auto_walk = true;
    ctx_.walk_mode = true;
  }
}

void ActorSystem::TeleportPlayer(f32 x, f32 y, f32 z) {
  if (player_actor_ < 0) return;
  Actor& a = actors_[player_actor_];
  if (a.character) physics_.SetCharacterPosition(a.character, Vec3{x, y, z});
  if (world::Transform* t = world_.Get<world::Transform>(a.entity)) {
    t->position[0] = x;
    t->position[1] = y;
    t->position[2] = z;
  }
  REC_INFO("quest: teleported player to ({:.1f}, {:.1f}, {:.1f})", x, y, z);
}

bool ActorSystem::LoadActorPart(const std::string& path, Actor& actor, i32 attach_bone) {
  auto bytes = vfs_.Read(asset::NormalizePath(path));
  if (!bytes) {
    REC_WARN("actor part not found: {}", path);
    return false;
  }
  ByteSpan span(bytes->data(), bytes->size());
  asset::AssetId id = asset::MakeAssetId(path);
  bethesda::NifConversion conv = bethesda::ConvertNifSkinnedMesh(span, id, path);
  bool rigid = false;
  if (!conv.mesh || !conv.mesh->skinned || conv.mesh->skin.bones.empty()) {
    // Head/hair are static meshes rigged to a single bone, not skinned.
    if (attach_bone < 0) {
      REC_WARN("actor part {} has no skinned geometry", path);
      return false;
    }
    conv = bethesda::ConvertNifRigid(span, id, path);
    if (!conv.mesh || conv.mesh->lods.empty() || conv.mesh->lods[0].vertices.empty()) {
      REC_WARN("actor part {} has no geometry", path);
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
    REC_INFO("actor part {}: rigid, riding bone {}", path, attach_bone);
  } else {
    part.skin = conv.mesh->skin;
    part.remap = anim::BuildBoneRemap(actor.skeleton, part.skin);
    u32 matched = 0;
    for (i32 b : part.remap) {
      if (b >= 0) ++matched;
    }
    REC_INFO("actor part {}: {} skin bones, {} matched to skeleton", path, part.skin.bones.size(),
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
    REC_ERROR("skeleton.nif not found in the mounted archives");
    return false;
  }
  asset::Skeleton skeleton;
  if (!bethesda::ConvertNifSkeleton(ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                    asset::MakeAssetId(skel_path), &skeleton)) {
    REC_ERROR("failed to parse skeleton.nif");
    return false;
  }
  out->skeleton = std::move(skeleton);
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
    REC_ERROR("no skyrim body parts loaded");
    return false;
  }
  // Head + hair ride the head bone (static meshes, no FaceGen morphs).
  i32 head_bone = out->skeleton.Find("NPC Head [Head]");
  LoadActorPart("meshes/actors/character/character assets/malehead.nif", *out, head_bone);
  for (const std::string& hair : FindHeadPartModels(/*hair=*/3, 24)) {
    if (LoadActorPart(hair, *out, head_bone)) {
      REC_INFO("hair: {}", hair);
      break;
    }
  }
  return true;
}

bool ActorSystem::LoadStarfieldActorPart(const std::string& path, Actor& actor,
                                         const bethesda::StarfieldMaterialDb& mat_db) {
  auto bytes = vfs_.Read(asset::NormalizePath(path));
  if (!bytes) {
    REC_WARN("starfield actor part not found: {}", path);
    return false;
  }
  ByteSpan span(bytes->data(), bytes->size());
  asset::AssetId id = asset::MakeAssetId(path);
  base::UniquePointer<asset::Mesh> mesh =
      bethesda::ConvertStarfieldSkinnedNif(*ctx_.assets, mat_db, span, id, path);
  if (!mesh || !mesh->skinned || mesh->skin.bones.empty()) {
    REC_WARN("starfield actor part {} has no skinned geometry", path);
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
  REC_INFO("starfield actor part {}: {} skin bones, {} matched to skeleton", path,
           part.skin.bones.size(), matched);
  actor.parts.push_back(std::move(part));
  return true;
}

bool ActorSystem::LoadStarfieldActorTemplate(Actor* out) {
  const std::string skel_path = "meshes/actors/human/characterassets/skeleton.nif";
  auto skel_bytes = vfs_.Read(asset::NormalizePath(skel_path));
  if (!skel_bytes) {
    REC_ERROR("starfield skeleton.nif not found in the mounted archives");
    return false;
  }
  asset::Skeleton skeleton;
  if (!bethesda::ConvertNifSkeleton(ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                    asset::MakeAssetId(skel_path), &skeleton)) {
    REC_ERROR("failed to parse starfield skeleton.nif");
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
    REC_ERROR("no starfield body parts loaded");
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
  REC_INFO("player: walkable avatar spawned at ({:.1f}, {:.1f}, {:.1f}); walk mode on", ground_pos.x,
           ground_pos.y, ground_pos.z);
}

bool ActorSystem::CreateSkyrimActor() {
  if (!SpawnPlayerActor({0, 0, 0})) return false;
  REC_INFO("loaded skyrim skeleton: {} bones", actors_[player_actor_].skeleton.bones.size());

  // A ground slab to stand on (top at y = 0, where the skeleton's feet sit),
  // plus a step under one foot so the foot IK has something to adapt to.
  asset::Mesh slab = asset::MakeCube(10.0f, asset::MakeAssetId("builtin/actor_ground"));
  asset::Mesh step = asset::MakeCube(0.45f, asset::MakeAssetId("builtin/actor_step"));
  renderer_.UploadMesh(slab);
  renderer_.UploadMesh(step);
  ecs::Entity ground = world_.Create();
  world_.Add(ground, world::Transform{.position = {0, -10.0f, 0}});
  world_.Add(ground, world::Renderable{slab.id});
  ecs::Entity step_e = world_.Create();
  world_.Add(step_e, world::Transform{.position = {0.5f, -0.33f, 0}});
  world_.Add(step_e, world::Renderable{step.id});
  if (physics_.initialized()) {
    physics_.AddStaticBox({0, -10.0f, 0}, {10, 10, 10});
    physics_.AddStaticBox({0.5f, -0.33f, 0}, {0.45f, 0.45f, 0.45f});
  }

  // The skeleton faces -Z in engine space, so frame it from the front.
  camera_.set_position({0.0f, 0.95f, -3.0f});
  camera_.set_yaw_pitch(3.14159f, -0.08f);
  REC_INFO("skyrim actor ready ({} body parts); press T to walk it",
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
  actor.locomotion.phase = anim::AdvancePhase(actor.locomotion.phase, actor.speed, dt);
  actor.locomotion.Apply(actor.skeleton, actor.speed, &actor.pose);

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
  actor.prev_model = model;
}

const ActorSystem::Actor* ActorSystem::SoldierTemplate(int team) {
  if (team != 1 && team != 2) return nullptr;
  std::optional<Actor>& slot = soldier_templates_[team - 1];
  if (!slot) {
    Actor tmpl;
    if (!LoadActorTemplate(&tmpl, team)) return nullptr;  // fall back to the bare body
    tmpl.animate = true;
    tmpl.speed = 0.0f;
    tmpl.foot_ik = false;
    slot = std::move(tmpl);
    REC_INFO("soldier template ready: team {} ({} parts)", team, slot->parts.size());
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
      REC_INFO("npc rendering: using the builtin biped (game body assets absent)");
    }
    tmpl.animate = true;
    tmpl.speed = 0.0f;     // idle
    tmpl.foot_ik = false;  // skip per-NPC ground raycasts
    npc_template_ = std::move(tmpl);
    REC_INFO("npc actor template ready ({} parts)", npc_template_->parts.size());
  }
  // Give every NPC entity without one a skinned actor instance (own pose, GPU
  // meshes shared by hash with the template). Battle actors (those on a combat
  // team) instance from their faction's armoured template instead of the bare
  // civilian body, so the two armies read as soldiers.
  world_.Each<world::Npc, world::Transform>([&](ecs::Entity e, world::Npc&, world::Transform&) {
    const u64 key = static_cast<u64>(e.generation) << 32 | e.index;
    if (npc_actors_.find(key)) return;
    const Actor* tmpl = &*npc_template_;
    if (const world::CombatTeam* ct = world_.Get<world::CombatTeam>(e))
      if (const Actor* st = SoldierTemplate(ct->team)) tmpl = st;
    Actor a = *tmpl;
    a.entity = e;
    a.character = 0;
    a.pose.ResetToBind(a.skeleton);
    npc_actors_.insert(key, std::move(a));
  });
  // Drop actors whose NPC entity streamed out, so none render at the origin.
  scratch_dead_actors_.clear();
  for (auto entry : npc_actors_)
    if (!world_.IsAlive(entry.value.entity)) scratch_dead_actors_.push_back(entry.key);
  for (u64 key : scratch_dead_actors_) npc_actors_.erase(key);
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
      [&](ecs::Entity e, world::Npc&, world::Transform& t) { ensure(e, t); });
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
    if (!world_.IsAlive(e)) scratch_dead_actors_.push_back(entry.key);
  }
  for (u64 key : scratch_dead_actors_) {
    if (physics::BodyId* body = solid_bodies_.find(key)) physics_.RemoveBody(*body);
    solid_bodies_.erase(key);
  }
}

}  // namespace rec

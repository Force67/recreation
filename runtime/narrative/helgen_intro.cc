#include "runtime/actor/actor_system.h"
#include "runtime/narrative/helgen_intro.h"

#include <base/algorithm.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/option.h>
#include <base/strings/xstring.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "components/bethesda/record.h"
#include "runtime/vehicle/cart_visuals.h"
#include "core/log.h"
#include "components/dialogue/dialogue.h"
#include "runtime/app/engine_context.h"
#include "runtime/camera/fly_camera.h"
#include "components/quest/package_record.h"
#include "components/script/games/skyrim/skyrim_bindings.h"
#include "components/script/games/skyrim/skyrim_condition_context.h"
#include "components/quest/quest_def.h"
#include "components/quest/scene_record.h"
#include "components/world/cell_streaming.h"
#include "components/world/components.h"

namespace rx {
namespace {

base::Option<bool> Intro{"helgen.intro", false, "RX_HELGEN_INTRO"};
// RX_HELGEN_CHASE frames the whole rig from outside instead of riding in the
// player's seat: the shot that shows the horse, the cart and who is aboard.
// Set it to "eye_x,eye_y,eye_z,at_x,at_y,at_z" (cart-local metres) to place that
// camera yourself, which is how the seats in the bed were lined up.
base::Option<const char*> Chase{"helgen.chase", nullptr, "RX_HELGEN_CHASE"};
// RX_HELGEN_SEAT rides in the cart instead, first person from the prisoner's
// seat: vanilla's framing, though this cart's slatted bed crowds the shot.
base::Option<bool> Seat{"helgen.seat", false, "RX_HELGEN_SEAT"};

constexpr f32 kPi = 3.14159265358979f;
// Bethesda Z-up game units -> engine Y-up metres, the engine's one conversion.
constexpr f32 kBethScale = 0.01428f;

Vec3 BethToEngine(f32 x, f32 y, f32 z) { return {x * kBethScale, z * kBethScale, -y * kBethScale}; }

// The engine yaw that turns a model's forward onto `dir`. A Bethesda model
// faces +Y, which the axis change above maps onto -Z, so everything placed here
// -- cart, horse, riders -- shares this one convention.
f32 FacingYaw(const Vec3& dir) { return std::atan2(-dir.x, -dir.z); }

// The quest whose opening this is, and the aliases inside it that name the
// pieces: the horse carries the route as its AI packages, the cart is towed, and
// the rest are the passengers. Nothing here is a coordinate -- the records hold
// the route, the cast and the dialogue.
constexpr const char* kQuest = "MQ101";
constexpr const char* kHorseAlias = "CartHorse1";

// The cart pulls up short of its last marker rather than nosing into whatever
// stands there, so the arrival has the town in frame.
constexpr f32 kStopShortOfEnd = 6.0f;  // metres

// The cart-prisoner clips are furniture animations: each carries its own seat as
// a baked-in offset on the actor's COM, authored against the cart furniture's
// origin rather than the static mesh's. Planting a rider on that origin and
// letting the clip do the rest is what puts them in their vanilla seats; the
// offset between the two origins was measured off the bed.
const Vec3 kFurnitureOrigin = BethToEngine(0, 60, 0);

// The player's own seat: the free corner across the bed from Ralof, taken from
// wherever his clip actually seats him rather than guessed at. Cart-local
// metres, +X being the far side of the bed from him.
const Vec3 kSeatFromRalof{0.92f, 0.02f, 0.10f};
// Fallback seat, cart-local, for a run with no riders (no game body assets).
const Vec3 kPlayerEyeFallback = BethToEngine(30, -150, 163);

// The cutscene's three cuts, as cart-local eye/look-at pairs in metres. The
// cart's forward is -Z, so a negative eye z stands ahead of the horse.
struct Shot {
  Vec3 eye, at;
};
constexpr Shot kShots[] = {
    {{-5.6f, 3.2f, 2.6f}, {0.0f, 1.4f, -2.6f}},  // alongside, following it down
    {{4.2f, 2.6f, -9.5f}, {0.0f, 1.6f, -1.0f}},  // ahead of the horse, looking back
    {{-4.6f, 2.7f, 3.4f}, {0.8f, 1.7f, -5.0f}},  // off the flank, past it to Helgen's gate
};

// Who rides the player's cart, each with the cart-prisoner idle the vanilla
// scene gives that seat. `alias` is the MQ101 quest alias, which is also how the
// scene records name the speaker.
struct RiderDef {
  const char* alias;  // MQ101 alias, which both fills the actor and names the speaker
  const char* clip;
  int outfit;  // ActorSystem soldier kind: 0 bare, 1 imperial, 2 stormcloak
  // Placed reference to fall back on when the alias is a find-matching one that
  // only fills once the quest is running. Drops out when aliases fill live.
  const char* fallback_ref;
};
constexpr const char* kClipDir = "meshes/actors/character/animations/";
constexpr RiderDef kRiders[] = {
    {"Ralof", "cartprisoneraidle.hkx", 2, nullptr},
    {"Ulfric", "cartprisonerbidle.hkx", 2, nullptr},
    {"Prisoner01", "cartprisonercidle.hkx", 0, nullptr},  // Lokir, the horse thief
    {"ImperialSoldier02", "carttraveldriveridle.hkx", 1, "MQ101CartDriverA"},
};

// The two MQ101 scenes that make up the ride: the cart, then rolling in.
constexpr const char* kSceneIds[] = {"MQ101Scene1", "MQ101Scene3"};

}  // namespace

HelgenIntro::HelgenIntro(EngineContext& ctx, ActorSystem* actors) : ctx_(ctx), actors_(actors) {
  enabled_ = Intro && !ctx.config->headless;
}

bool HelgenIntro::StartCell(i32* cell_x, i32* cell_y) {
  if (!enabled_ || !BuildRoute()) return false;
  constexpr f32 kCellSize = 4096.0f;  // game units
  const Vec3 start = route_[0];
  *cell_x = static_cast<i32>(std::floor(start.x / kBethScale / kCellSize));
  *cell_y = static_cast<i32>(std::floor(-start.z / kBethScale / kCellSize));
  return true;
}

bool HelgenIntro::StartView(Vec3* eye, Vec3* target) {
  if (!enabled_ || !BuildRoute() || route_.size() < 2) return false;
  const Vec3 forward = Normalize(Vec3{route_[1].x - route_[0].x, 0, route_[1].z - route_[0].z});
  *eye = Vec3{route_[0].x, 2.0f, route_[0].z};
  *target = *eye + forward * 10.0f;
  return true;
}

bool HelgenIntro::BuildRoute() {
  if (!route_.empty()) return true;
  if (!ctx_.records) return false;

  // The route is the quest's own: find MQ101, take the cart horse's alias, and
  // read the travel packages it stacks on the horse. Each leg names a marker,
  // and each marker heads a chain of unnamed ones that shape the path between.
  bethesda::GlobalFormId quest_id{};
  ctx_.records->EachOfType(
      FourCc('Q', 'U', 'S', 'T'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
        if (quest_id.local_id != 0) return;
        bethesda::Record r;
        if (ctx_.records->Parse(id, &r) && r.GetString(FourCc('E', 'D', 'I', 'D')) == kQuest)
          quest_id = id;
      });
  bethesda::Record quest_record;
  if (quest_id.local_id == 0 || !ctx_.records->Parse(quest_id, &quest_record)) {
    RX_WARN("helgen: no {} quest record, cannot resolve the cart route", kQuest);
    return false;
  }
  quest_ = quest::ParseQuestDefinition(quest_id.packed(), quest_record, ctx_.strings);
  quest_plugin_ = quest_id.plugin;
  quest_handle_ = quest_id.packed();

  const quest::AliasDef* horse = nullptr;
  for (const quest::AliasDef& alias : quest_.aliases)
    if (alias.name == kHorseAlias) horse = &alias;
  if (!horse) {
    RX_WARN("helgen: {} has no '{}' alias", kQuest, kHorseAlias);
    return false;
  }
  const base::Vector<quest::RouteStop> stops =
      quest::ResolveAliasTravelRoute(*ctx_.records, *horse, quest_plugin_);
  if (stops.size() < 2) {
    RX_WARN("helgen: {}'s route resolved to {} stops", kHorseAlias, stops.size());
    return false;
  }
  for (const quest::RouteStop& stop : stops)
    route_.push_back(BethToEngine(stop.position[0], stop.position[1], stop.position[2]));

  route_arc_.push_back(0);
  for (size_t i = 1; i < route_.size(); ++i) {
    const Vec3 d = route_[i] - route_[i - 1];
    route_arc_.push_back(route_arc_[i - 1] + std::sqrt(d.x * d.x + d.z * d.z));
  }
  route_length_ = route_arc_[route_arc_.size() - 1] - kStopShortOfEnd;

  // Keep the horse's packages in record order for the selector, noting where
  // each destination falls along the route and the journal stage it waits for.
  for (u32 raw : horse->package_raw) {
    const bethesda::GlobalFormId pack =
        ctx_.records->ResolveFrom(bethesda::RawFormId{raw}, quest_plugin_);
    bethesda::Record prec;
    if (!ctx_.records->Parse(pack, &prec)) continue;
    Leg leg;
    leg.def = quest::ParsePackageRecord(pack.packed(), prec, *ctx_.records);
    leg.arc = route_length_;  // no destination on the route: no limit
    for (size_t i = 0; i < stops.size(); ++i)
      if (stops[i].package == leg.def.handle) leg.arc = route_arc_[i];
    // The stage the package waits for: its first "GetStage >= N" gate.
    for (const quest::Comparison& c : leg.def.conditions.comparisons) {
      if (c.func == quest::Func::kGetStage && c.op == quest::CompareOp::kGreaterOrEqual) {
        leg.stage = static_cast<i32>(c.value);
        break;
      }
    }
    legs_.push_back(base::move(leg));
  }
  RX_INFO("helgen: route from {}.{} packages: {} points, {:.0f} m, {} legs", kQuest, kHorseAlias,
          route_.size(), route_length_, legs_.size());
  return true;
}

f32 HelgenIntro::PackageLimit() {
  // Without the live bindings there is no quest state to gate on, so the ride
  // just runs the whole authored route.
  if (!ctx_.bindings || legs_.empty()) return route_length_;

  script::skyrim::SkyrimConditionContext conditions(ctx_.bindings);
  base::Vector<quest::PackageDef> defs;
  defs.reserve(legs_.size());
  for (const Leg& leg : legs_) defs.push_back(leg.def);
  const int active = quest::SelectActivePackage(defs, conditions);
  if (active < 0) return route_length_;  // quest not at the ride: no gate

  if (active != active_leg_) {
    active_leg_ = active;
    RX_INFO("helgen: package {} active at stage {}, route limit {:.0f} m", active,
            ctx_.bindings->GetStage(script::papyrus::ObjectRef{quest_handle_}),
            legs_[static_cast<size_t>(active)].arc);
  }
  return legs_[static_cast<size_t>(active)].arc;
}

Vec3 HelgenIntro::RouteSample(f32 arc, Vec3* forward) const {
  const size_t last = route_.size() - 1;
  arc = base::Clamp(arc, 0.0f, route_length_);
  size_t i = 0;
  while (i + 1 < last && route_arc_[i + 1] < arc) ++i;
  const f32 span = base::Max(route_arc_[i + 1] - route_arc_[i], 1e-3f);
  const f32 t = base::Clamp((arc - route_arc_[i]) / span, 0.0f, 1.0f);
  if (forward)
    *forward = Normalize(Vec3{route_[i + 1].x - route_[i].x, 0, route_[i + 1].z - route_[i].z});
  return route_[i] + (route_[i + 1] - route_[i]) * t;
}

f32 HelgenIntro::GroundY(f32 x, f32 z, f32 fallback) const {
  f32 y = fallback;
  if (ctx_.streamer && ctx_.streamer->GroundHeight(x, z, &y)) return y;
  return fallback;
}

void HelgenIntro::LoadDialogue() {
  if (!ctx_.records) return;

  bethesda::GlobalFormId scenes[2]{};
  ctx_.records->EachOfType(
      FourCc('S', 'C', 'E', 'N'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
        bethesda::Record r;
        if (!ctx_.records->Parse(id, &r)) return;
        const base::String edid = r.GetString(FourCc('E', 'D', 'I', 'D'));
        for (int i = 0; i < 2; ++i)
          if (scenes[i].local_id == 0 && edid == kSceneIds[i]) scenes[i] = id;
      });

  quest::QuestDef quest;
  i32 phase_base = 0;
  for (const bethesda::GlobalFormId& scene : scenes) {
    if (scene.local_id == 0) continue;
    bethesda::Record record;
    if (!ctx_.records->Parse(scene, &record)) continue;
    const quest::SceneDef def = quest::ParseSceneRecord(scene.packed(), record, ctx_.records);
    // The owning quest's aliases are what name the speakers.
    if (quest.handle == 0 && def.quest != 0) {
      const bethesda::GlobalFormId qid{static_cast<u16>(def.quest >> 32),
                                       static_cast<u32>(def.quest)};
      bethesda::Record qrec;
      if (ctx_.records->Parse(qid, &qrec))
        quest = quest::ParseQuestDefinition(def.quest, qrec, ctx_.strings);
    }
    i32 max_phase = 0;
    for (const quest::SceneActionDef& action : def.actions) {
      max_phase = base::Max(max_phase, action.start_phase);
      if (action.kind != quest::SceneActionDef::Kind::kDialogue || action.topic == 0) continue;
      const bethesda::GlobalFormId dial{static_cast<u16>(action.topic >> 32),
                                        static_cast<u32>(action.topic)};
      const dialogue::Topic topic = dialogue::ParseTopic(*ctx_.records, dial, ctx_.strings);
      // A topic can carry several conditioned variants of the same beat (race
      // and gender swaps); the scene only plays one, so take the first.
      for (const dialogue::Response& response : topic.responses) {
        if (response.npc_line.empty()) continue;
        const quest::AliasDef* alias = quest.FindAlias(action.actor_alias);
        Line line;
        line.phase = phase_base + action.start_phase;
        line.speaker = alias && !alias->name.empty() ? alias->name : base::String("Prisoner");
        line.text = response.npc_line;
        lines_.push_back(base::move(line));
        break;
      }
    }
    phase_base += max_phase + 1;  // the second scene continues after the first
  }
  std::stable_sort(lines_.begin(), lines_.end(),
                   [](const Line& a, const Line& b) { return a.phase < b.phase; });

  // Pace each line by how long it takes to read, with a beat between them.
  f32 t = 4.0f;  // lead-in, so the ride starts before anyone speaks
  for (Line& line : lines_) {
    const f32 read = 1.5f + 0.055f * static_cast<f32>(line.text.size());
    line.start = t;
    line.end = t + base::Clamp(read, 2.4f, 7.5f);
    t = line.end + 0.45f;
  }
  RX_INFO("helgen: {} spoken lines read from the MQ101 scenes, {:.0f}s of dialogue", lines_.size(),
          t);
}

bool HelgenIntro::WorldReady() const {
  if (!ctx_.streamer) return false;
  // The road has to exist before the cart can ride it, and the cells around the
  // start have to be in before the first frame is worth showing.
  f32 ground = 0;
  if (!ctx_.streamer->GroundHeight(route_[0].x, route_[0].z, &ground)) return false;
  return ctx_.streamer->caught_up() || wait_time_ > 30.0f;
}

void HelgenIntro::Spawn() {
  LoadDialogue();
  // Pace the cart so it rolls through the gate on the closing exchange rather
  // than arriving early and waiting there.
  const f32 talking = lines_.empty() ? 60.0f : lines_.back().end;
  speed_ = base::Clamp(route_length_ / base::Max(talking - 9.0f, 20.0f), 1.2f, 4.0f);

  Vec3 forward;
  const Vec3 start = RouteSample(0, &forward);
  const f32 yaw = FacingYaw(forward);

  asset::AssetId cart_mesh;
  if (!cart::BakeBody(ctx_.assets, ctx_.renderer, cart::kBodyMesh, false, "helgen/cart",
                      &cart_mesh)) {
    cart_mesh = cart::MakeBox(ctx_.renderer, "helgen/cart_box", {1.2f, 0.7f, 2.2f}, 0.35f, 0.22f,
                              0.12f, true)
                    .id;
  }
  cart_entity_ = ctx_.world->Create();
  ctx_.world->Add(cart_entity_, world::Transform{});
  ctx_.world->Add(cart_entity_, world::Renderable{cart_mesh});

  // The horse: the game's creature rig walking its forward cycle in place while
  // the route tows it.
  horse_entity_ =
      actors_->SpawnCreatureNpc("horse", "meshes/actors/horse/animations/walkforward.hkx",
                                Vec3{start.x, GroundY(start.x, start.z, start.y), start.z}, yaw);

  for (const RiderDef& rider : kRiders) {
    bethesda::GlobalFormId npc = AliasActor(rider.alias);
    if (npc.local_id == 0 && rider.fallback_ref) npc = RefBase(rider.fallback_ref);
    if (npc.local_id == 0) {
      RX_WARN("helgen: {} alias '{}' filled nothing", kQuest, rider.alias);
      continue;
    }
    const ecs::Entity e = actors_->SpawnScriptedNpc(npc, kClipDir + base::String(rider.clip), start,
                                                    yaw, rider.outfit);
    if (!ctx_.world->IsAlive(e)) continue;
    riders_.push_back({e, rider.alias});
  }

  PlaceCart();
  RX_INFO("helgen: cart on the road, {:.0f} m to the square at {:.1f} m/s, {} aboard",
          route_length_, speed_, riders_.size());
}

void HelgenIntro::PlaceCart() {
  Vec3 forward;
  const Vec3 p = RouteSample(arc_, &forward);
  const f32 yaw = FacingYaw(forward);
  // Sit the wheels on the terrain and pitch the bed along the road's grade, so
  // the cart leans into the descent instead of floating level over it.
  const f32 behind = GroundY(p.x - forward.x * 1.4f, p.z - forward.z * 1.4f, p.y);
  const f32 ahead = GroundY(p.x + forward.x * 1.4f, p.z + forward.z * 1.4f, p.y);
  cart_pos_ = Vec3{p.x, (ahead + behind) * 0.5f, p.z};
  cart_rot_ = QuatFromAxisAngle({0, 1, 0}, yaw) *
              QuatFromAxisAngle({1, 0, 0}, -std::atan2(ahead - behind, 2.8f));

  if (world::Transform* t = ctx_.world->Get<world::Transform>(cart_entity_)) {
    t->position[0] = cart_pos_.x;
    t->position[1] = cart_pos_.y;
    t->position[2] = cart_pos_.z;
    t->rotation[0] = cart_rot_.x;
    t->rotation[1] = cart_rot_.y;
    t->rotation[2] = cart_rot_.z;
    t->rotation[3] = cart_rot_.w;
  }

  // The horse walks a cart-length ahead, on its own bit of ground.
  if (ctx_.world->IsAlive(horse_entity_)) {
    Vec3 horse_forward;
    const Vec3 hp = RouteSample(arc_ + 4.6f, &horse_forward);
    const f32 horse_yaw = FacingYaw(horse_forward);
    if (world::Transform* t = ctx_.world->Get<world::Transform>(horse_entity_)) {
      t->position[0] = hp.x;
      t->position[1] = GroundY(hp.x, hp.z, hp.y);
      t->position[2] = hp.z;
      t->rotation[0] = 0;
      t->rotation[1] = std::sin(horse_yaw * 0.5f);
      t->rotation[2] = 0;
      t->rotation[3] = std::cos(horse_yaw * 0.5f);
    }
    actors_->SetNpcGait(horse_entity_, arc_ < route_length_ ? speed_ : 0.0f, false, horse_yaw);
  }

  // Every rider is planted on the cart's furniture origin with the cart's own
  // facing; their clip carries them from there into their seat.
  const Vec3 furniture = CartLocal(kFurnitureOrigin);
  for (const Passenger& rider : riders_) {
    world::Transform* t = ctx_.world->Get<world::Transform>(rider.entity);
    if (!t) continue;
    t->position[0] = furniture.x;
    t->position[1] = furniture.y;
    t->position[2] = furniture.z;
    t->rotation[0] = 0;
    t->rotation[1] = std::sin(yaw * 0.5f);
    t->rotation[2] = 0;
    t->rotation[3] = std::cos(yaw * 0.5f);
  }
}

bethesda::GlobalFormId HelgenIntro::RefBase(const base::String& editor_id) const {
  bethesda::GlobalFormId base{};
  ctx_.records->EachOfType(
      FourCc('A', 'C', 'H', 'R'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        if (base.local_id != 0) return;
        bethesda::Record achr;
        if (!ctx_.records->Parse(id, &achr)) return;
        if (achr.GetString(FourCc('E', 'D', 'I', 'D')) != editor_id) return;
        const bethesda::Subrecord* name = achr.Find(FourCc('N', 'A', 'M', 'E'));
        if (!name || name->data.size() < 4) return;
        u32 raw;
        std::memcpy(&raw, name->data.data(), 4);
        base = ctx_.records->ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin);
      });
  return base;
}

bethesda::GlobalFormId HelgenIntro::AliasActor(const base::String& name) const {
  // A reference alias names its actor one of two ways: a forced reference (a
  // placed ACHR, whose NAME is the NPC_) or a unique-actor base directly.
  for (const quest::AliasDef& alias : quest_.aliases) {
    if (alias.name != name) continue;
    if (alias.unique_actor_raw)
      return ctx_.records->ResolveFrom(bethesda::RawFormId{alias.unique_actor_raw}, quest_plugin_);
    if (!alias.forced_ref_raw) break;
    const bethesda::GlobalFormId ref =
        ctx_.records->ResolveFrom(bethesda::RawFormId{alias.forced_ref_raw}, quest_plugin_);
    bethesda::Record achr;
    if (!ctx_.records->Parse(ref, &achr)) break;
    const bethesda::Subrecord* base = achr.Find(FourCc('N', 'A', 'M', 'E'));
    if (!base || base->data.size() < 4) break;
    u32 raw;
    std::memcpy(&raw, base->data.data(), 4);
    const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(ref);
    return ctx_.records->ResolveFrom(bethesda::RawFormId{raw},
                                     stored ? stored->winning_plugin : quest_plugin_);
  }
  return {};
}

Vec3 HelgenIntro::CartLocal(const Vec3& offset) const {
  return cart_pos_ + Rotate(cart_rot_, offset);
}

bool HelgenIntro::RiderHead(const base::String& alias, Vec3* out) const {
  for (const Passenger& rider : riders_)
    if (rider.alias == alias) return actors_->NpcHeadWorld(rider.entity, out);
  return false;
}

void HelgenIntro::DriveCamera() {
  Vec3 eye, target;
  if (const char* chase = Chase.get(); chase && chase[0]) {
    // Hand-placed shot, for lining the rig up.
    f32 c[6] = {-5.2f, 3.0f, 3.2f, 0.0f, 1.3f, -1.6f};
    std::sscanf(chase, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]);
    eye = CartLocal({c[0], c[1], c[2]});
    target = CartLocal({c[3], c[4], c[5]});
  } else if (Seat) {
    // Riding in the cart: the free corner across the bed from Ralof, wherever
    // his clip seated him, looking at whoever is speaking.
    if (!RiderHead("Ralof", &eye)) eye = CartLocal(kPlayerEyeFallback);
    else eye = eye + Rotate(cart_rot_, kSeatFromRalof);
    eye = eye + Rotate(cart_rot_, Vec3{std::sin(arc_ * 1.7f) * 0.02f, 0, 0});
    const Line* line =
        line_ < lines_.size() && time_ >= lines_[line_].start ? &lines_[line_] : nullptr;
    if (!line || !RiderHead(line->speaker, &target)) target = RoadAhead();
  } else {
    // Three cuts over the ride: alongside the cart on the mountain road, then
    // ahead of the horse looking back at it coming down, then high behind as it
    // rolls into Helgen.
    const f32 progress = route_length_ > 0 ? arc_ / route_length_ : 0.0f;
    const Shot& shot = progress < 0.34f ? kShots[0] : progress < 0.68f ? kShots[1] : kShots[2];
    eye = CartLocal(shot.eye);
    target = CartLocal(shot.at);
    // A shot placed off the cart can end up inside the mountain the road cuts
    // through; keep it above whatever ground it is standing over.
    eye.y = base::Max(eye.y, GroundY(eye.x, eye.z, eye.y) + 1.4f);
  }

  // Ease onto the target so a cut turns the camera instead of snapping it.
  if (!cam_target_valid_) {
    cam_target_ = target;
    cam_target_valid_ = true;
  }
  cam_target_ = cam_target_ + (target - cam_target_) * 0.08f;

  ctx_.walk_eye = eye;
  ctx_.walk_target = cam_target_;
  if (ctx_.camera) {
    ctx_.camera->set_position(eye);
    const Vec3 d = Normalize(cam_target_ - eye);
    ctx_.camera->set_yaw_pitch(std::atan2(d.x, -d.z), std::asin(base::Clamp(d.y, -1.0f, 1.0f)));
  }
}

Vec3 HelgenIntro::RoadAhead() const {
  Vec3 forward;
  const Vec3 ahead = RouteSample(arc_ + 22.0f, &forward);
  return {ahead.x, GroundY(ahead.x, ahead.z, ahead.y) + 2.4f, ahead.z};
}

void HelgenIntro::UpdateOverlay() {
  overlay_ = TrailerOverlay{};
  overlay_.active = true;
  if (stage_ == Stage::kWaitForWorld) {
    overlay_.fade = 1.0f;
    overlay_.loading = true;
    overlay_.loading_label = "HELGEN";
    return;
  }
  overlay_.letterbox = base::Clamp(time_ / 1.5f, 0.0f, 1.0f);
  overlay_.fade = stage_ == Stage::kArrived ? base::Clamp((time_ - arrive_time_) / 3.0f, 0.0f, 1.0f)
                                            : base::Clamp(1.0f - time_ / 2.5f, 0.0f, 1.0f);
  if (time_ < 7.0f) {
    overlay_.intro_title = "HELGEN";
    overlay_.intro_subtitle = "SKYRIM - UNBOUND";
    overlay_.intro_alpha = time_ < 5.0f ? base::Clamp(time_ / 1.5f, 0.0f, 1.0f)
                                        : base::Clamp((7.0f - time_) / 2.0f, 0.0f, 1.0f);
  }
  if (line_ < lines_.size()) {
    const Line& line = lines_[line_];
    if (time_ >= line.start) {
      overlay_.caption_speaker = line.speaker;
      overlay_.caption = line.text;
      overlay_.caption_alpha =
          base::Min(base::Min((time_ - line.start) / 0.35f, (line.end - time_) / 0.35f), 1.0f);
    }
  }
}

void HelgenIntro::Advance(f32 dt) {
  time_ += dt;
  // How far the horse's currently-active AI package lets it travel. Reaching a
  // leg's destination advances the journal to the next leg's stage, which is
  // what that package's own fragment does in the game.
  const f32 limit = PackageLimit();
  arc_ = base::Min(arc_ + speed_ * dt, limit);
  if (ctx_.bindings && arc_ >= limit - 0.05f && limit < route_length_ - 0.05f) {
    i32 next = -1;
    for (const Leg& leg : legs_)
      if (leg.stage > 0 && leg.arc > limit + 0.05f && (next < 0 || leg.stage < next))
        next = leg.stage;
    // Only once per leg: the arrival test stays true for the frames before the
    // package selection catches up, and stage fragments have side effects.
    if (next > 0 && next != advanced_stage_) {
      advanced_stage_ = next;
      RX_INFO("helgen: arrived, advancing {} to stage {}", kQuest, next);
      ctx_.bindings->SetStage(script::papyrus::ObjectRef{quest_handle_}, next);
    }
  }
  while (line_ < lines_.size() && time_ > lines_[line_].end) ++line_;
  if (stage_ == Stage::kRide && arc_ >= route_length_ && line_ >= lines_.size()) {
    stage_ = Stage::kArrived;
    arrive_time_ = time_;
    RX_INFO("helgen: the cart is in, {:.0f}s", time_);
  }
  if (stage_ == Stage::kArrived && time_ > arrive_time_ + 3.5f) {
    stage_ = Stage::kDone;
    RX_INFO("helgen: intro complete");
  }
}

bool HelgenIntro::Update(f32 dt) {
  if (!enabled_ || stage_ == Stage::kDone) return false;
  ctx_.walk_mode = false;  // the cutscene owns the camera, not a player capsule

  if (stage_ == Stage::kWaitForWorld) {
    wait_time_ += dt;
    // Park the camera at the start so streaming pulls the road in around it.
    Vec3 eye, target;
    if (!StartView(&eye, &target)) return false;
    eye.y = GroundY(eye.x, eye.z, eye.y) + 2.0f;
    if (ctx_.camera) {
      ctx_.camera->set_position(eye);
      const Vec3 d = Normalize(Vec3{target.x - eye.x, 0, target.z - eye.z});
      ctx_.camera->set_yaw_pitch(std::atan2(d.x, -d.z), -0.05f);
    }
    if (WorldReady()) {
      Spawn();
      stage_ = Stage::kRide;
      time_ = 0;
    }
    UpdateOverlay();
    return true;
  }

  Advance(dt);
  PlaceCart();
  DriveCamera();
  UpdateOverlay();
  return true;
}

}  // namespace rx

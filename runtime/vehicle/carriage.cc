#include "runtime/vehicle/carriage.h"

#include <base/algorithm.h>
#include <base/memory/move.h>
#include <base/option.h>
#include <base/strings/to_string.h>

#include <cmath>
#include <cstring>

#include "components/world/components.h"
#include "core/input.h"
#include "core/log.h"
#include "runtime/actor/actor_system.h"
#include "runtime/actor/npc_director.h"
#include "runtime/app/engine_context.h"
#include "runtime/camera/fly_camera.h"
#include "runtime/input/game_input.h"

namespace rx {

// RX_CARRIAGE_RIDE=<hold>: board the first carriage that streams in and set off
// for the hold whose name contains this, so a ride can be driven from a capture
// script instead of by hand.
static base::Option<const char*> RideTo{"carriage.ride", nullptr, "RX_CARRIAGE_RIDE"};

namespace {

constexpr f32 kUnitsToMeters = 0.01428f;
// Where the shaft ends on the horse: behind the animal, at hitch height, so the
// tongue pulls level from the mark it stands on.
constexpr f32 kHitchBehindHorse = 1.1f;
// A seated passenger's eyes above the seat piece's origin. The furniture marker
// in the model puts a rider's root about 0.85 m up, in the cart bed; sitting on
// the bench from there looks out over the driver without pressing the view into
// the bench back behind it.
constexpr f32 kSeatEyeHeight = 1.5f;
constexpr f32 kLookSensitivity = 0.0025f;  // radians per mouse pixel
constexpr size_t kMaxOfferedRoutes = 6;    // as many as there are number keys
constexpr f32 kTrotSpeed = 3.4f;           // m/s, the pace a carriage horse keeps
constexpr f32 kGallopSpeed = 9.0f;         // m/s, the cap on how fast the hitch can pull
constexpr f32 kMarkReached = 5.0f;         // m; the route's marks are road-coarse
constexpr f32 kRigFootprint = 6.0f;        // m; how far a loose piece can stand and still belong
// The driver's idle is a furniture animation: it carries his seat as a baked
// offset on the COM, authored against the cart, so planting him on the cart
// piece's origin and holding the clip puts him on the bench.
const char* kDriverIdle = "meshes/actors/character/animations/carttraveldriveridle.hkx";

Vec3 GameToEngine(const f32 p[3]) {
  return {p[0] * kUnitsToMeters, p[2] * kUnitsToMeters, -p[1] * kUnitsToMeters};
}

// Actor facing convention: a Bethesda model faces +Y, which the axis change maps
// to engine -Z, so forward at yaw 0 is -Z.
Vec3 ForwardOf(f32 yaw) {
  return {-std::sin(yaw), 0, -std::cos(yaw)};
}
f32 YawOfForward(const Vec3& forward) {
  return std::atan2(-forward.x, -forward.z);
}
f32 YawOf(const Quat& q) {
  return 2.0f * std::atan2(q.y, q.w);
}
void SetYaw(f32 rot[4], f32 yaw) {
  const f32 h = yaw * 0.5f;
  rot[0] = 0;
  rot[1] = std::sin(h);
  rot[2] = 0;
  rot[3] = std::cos(h);
}

Vec3 Flatten(const Vec3& v) {
  return {v.x, 0, v.z};
}

// The directory a base object's model lives in, lower case with forward
// slashes: "furniture/cart/" for every piece of a Skyrim carriage.
base::String ModelFolder(const bethesda::Record& base) {
  base::String path = base.GetString(FourCc('M', 'O', 'D', 'L'));
  for (char& c : path)
    c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t slash = path.rfind('/');
  return slash == base::String::npos ? base::String() : path.substr(0, slash + 1);
}

}  // namespace

CarriageSystem::CarriageSystem(EngineContext& ctx, ActorSystem* actors, NpcDirector* npc)
    : ctx_(ctx), actors_(actors), npc_(npc) {
  label_ = "Ride carriage";
  ctx_.carriage_activate = [this](u64 handle) { return Activate(handle); };
  ctx_.carriage_label = [this](u64 handle) { return Label(handle); };
}

ecs::Entity CarriageSystem::EntityFor(u64 ref) const {
  if (!ref || !ctx_.quest_world)
    return ecs::Entity{};
  const ecs::Entity e = ctx_.quest_world->Find(ref);
  return ctx_.world->IsAlive(e) ? e : ecs::Entity{};
}

bool CarriageSystem::RefPose(u64 ref, Vec3* position, Quat* rotation) const {
  const ecs::Entity e = EntityFor(ref);
  const world::Transform* t = ctx_.world->Get<world::Transform>(e);
  if (!t)
    return false;
  *position = Vec3{t->position[0], t->position[1], t->position[2]};
  *rotation = Quat{t->rotation[0], t->rotation[1], t->rotation[2], t->rotation[3]};
  return true;
}

bool CarriageSystem::CartForward(const Carriage& carriage, Vec3* forward) const {
  Vec3 cart, harness;
  Quat unused;
  if (!RefPose(carriage.refs.cart, &cart, &unused) ||
      !RefPose(carriage.refs.harness, &harness, &unused))
    return false;
  const Vec3 axis = Flatten(harness - cart);
  if (Length(axis) < 0.5f)
    return false;
  *forward = Normalize(axis);
  return true;
}

void CarriageSystem::Discover() {
  if (!ctx_.records || !ctx_.quest_world)
    return;
  if (!keywords_resolved_) {
    keywords_ = world::FindCarriageKeywords(*ctx_.records);
    keywords_resolved_ = true;
    if (!keywords_.valid())
      return;
  }
  if (!keywords_.valid())
    return;

  ctx_.world->Each<world::Npc, world::FormLink>(
      [&](ecs::Entity, world::Npc&, world::FormLink& link) {
        const u64 handle = link.form.packed();
        if (examined_.contains(handle))
          return;
        examined_.insert(handle);
        const world::CarriageRefs refs = world::ResolveCarriage(*ctx_.records, keywords_, handle);
        if (!refs.valid())
          return;
        Carriage carriage;
        carriage.refs = refs;
        carriages_.push_back(base::move(carriage));
        RX_INFO("carriage: driver 0x{:x} drives horse 0x{:x} on cart 0x{:x}", refs.driver,
                refs.horse, refs.cart);
      });
}

void CarriageSystem::Park(Carriage& carriage) {
  // The horse takes its harness mark (what its CarriageSitTarget package does)
  // and the driver his bench; both face the way the cart is pointed.
  Vec3 forward;
  if (!CartForward(carriage, &forward))
    return;
  const f32 yaw = YawOfForward(forward);

  Vec3 harness;
  Quat harness_rot;
  const ecs::Entity horse = EntityFor(carriage.refs.horse);
  if (ctx_.world->IsAlive(horse) && RefPose(carriage.refs.harness, &harness, &harness_rot)) {
    if (world::Transform* t = ctx_.world->Get<world::Transform>(horse)) {
      t->position[0] = harness.x;
      t->position[1] = harness.y;
      t->position[2] = harness.z;
      SetYaw(t->rotation, yaw);
    }
    if (!ctx_.world->Has<world::Seated>(horse))
      ctx_.world->Add(horse, world::Seated{});
    if (!carriage.horse_seated)
      carriage.horse_seated = actors_->PlayNpcGait(horse, false);
    actors_->SetNpcGait(horse, 0.0f, true, yaw);
    carriage.horse_walking = false;
  }

  Vec3 cart;
  Quat cart_rot;
  const ecs::Entity driver = EntityFor(carriage.refs.driver);
  if (ctx_.world->IsAlive(driver) && RefPose(carriage.refs.cart, &cart, &cart_rot)) {
    if (world::Transform* t = ctx_.world->Get<world::Transform>(driver)) {
      t->position[0] = cart.x;
      t->position[1] = cart.y;
      t->position[2] = cart.z;
      SetYaw(t->rotation, yaw);
    }
    if (!ctx_.world->Has<world::Seated>(driver))
      ctx_.world->Add(driver, world::Seated{});
    if (!carriage.driver_seated)
      carriage.driver_seated = actors_->PlayNpcClip(driver, kDriverIdle);
    actors_->SetNpcGait(driver, 0.0f, true, yaw);
  }
}

base::String CarriageSystem::ModelFolderOfBase(bethesda::GlobalFormId base) const {
  bethesda::Record record;
  return ctx_.records->Parse(base, &record) ? ModelFolder(record) : base::String();
}

base::String CarriageSystem::ModelFolderOfRef(u64 ref) const {
  const bethesda::GlobalFormId id{static_cast<u16>(ref >> 32), static_cast<u32>(ref)};
  bethesda::Record refr;
  if (!ctx_.records->Parse(id, &refr))
    return {};
  const bethesda::Subrecord* name = refr.Find(FourCc('N', 'A', 'M', 'E'));
  if (!name || name->data.size() < 4)
    return {};
  u32 raw = 0;
  std::memcpy(&raw, name->data.data(), 4);
  const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(id);
  return ModelFolderOfBase(ctx_.records->ResolveFrom(bethesda::RawFormId{raw},
                                                     stored ? stored->winning_plugin : 0));
}

void CarriageSystem::AddLooseArt(const Carriage& carriage,
                                 const Vec3& cart,
                                 base::Vector<Part>* parts) {
  // The rest of the carriage's art. Skyrim links the furniture pieces to their
  // driver but leaves the wagon body beside them as a plain static that nothing
  // points at, so the linked graph alone tows an invisible cart. Anything from
  // the same model folder standing inside the rig's footprint is part of it.
  if (!ctx_.records)
    return;
  const base::String folder = ModelFolderOfRef(carriage.refs.cart);
  if (folder.empty())
    return;
  ctx_.world->Each<world::Prop, world::Transform>(
      [&](ecs::Entity e, world::Prop& prop, world::Transform& t) {
        const Vec3 position{t.position[0], t.position[1], t.position[2]};
        if (Length(position - cart) > kRigFootprint)
          return;
        for (const Part& part : *parts)
          if (EntityFor(part.ref) == e)
            return;
        const world::FormLink* link = ctx_.world->Get<world::FormLink>(e);
        if (!link || ModelFolderOfBase(prop.base) != folder)
          return;
        Part part;
        part.ref = link->form.packed();
        part.local = position;
        part.local_rot = Quat{t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]};
        parts->push_back(part);
        RX_INFO("carriage: piece 0x{:x} rides along", part.ref);
      });
}

bool CarriageSystem::Depart(Carriage& carriage, size_t index) {
  if (carriage.driving || index >= carriage.routes.size() || !ctx_.physics)
    return false;
  // Every piece has to be in the world before the rig comes off its placement:
  // one that streams in afterwards has no recorded place on the chassis, and
  // guessing one would leave a bench standing in the road.
  base::Vector<Part> parts;
  for (u64 ref : {carriage.refs.cart, carriage.refs.seat, carriage.refs.harness}) {
    if (!ref)
      continue;
    Part part;
    part.ref = ref;
    if (!RefPose(ref, &part.local, &part.local_rot))
      return false;
    parts.push_back(part);
  }
  Vec3 forward;
  Vec3 cart, harness;
  Quat cart_rot, harness_rot;
  if (!CartForward(carriage, &forward) || !RefPose(carriage.refs.cart, &cart, &cart_rot) ||
      !RefPose(carriage.refs.harness, &harness, &harness_rot))
    return false;
  AddLooseArt(carriage, cart, &parts);

  // Chassis: a rolling proxy through the middle of the cart, deliberately
  // shorter than the art. A box as long as the carriage bellies out on every
  // rise in the road and wedges there; the art is pinned to the chassis below,
  // so the proxy only has to roll.
  Vec3 seat = cart;
  Quat seat_rot;
  const Vec3 centre = RefPose(carriage.refs.seat, &seat, &seat_rot) ? (cart + seat) * 0.5f : cart;

  world::CarriageConfig cfg;
  // The shaft reaches from the chassis to just behind the horse on its mark, so
  // the rig holds the shape the placements describe.
  cfg.tongue_z = base::Max(Dot(harness - centre, forward) - kHitchBehindHorse, cfg.half_extent.z);
  cfg.rest_length = 0.4f;

  const Vec3 spawn{centre.x, centre.y + cfg.wheel_radius + cfg.half_extent.y + 0.2f, centre.z};
  if (!carriage.rig.Spawn(*ctx_.physics, spawn, std::atan2(forward.x, forward.z), cfg)) {
    RX_WARN("carriage: could not create the physics cart for 0x{:x}", carriage.refs.cart);
    return false;
  }

  Vec3 chassis_pos;
  f32 chassis_rot[4];
  if (!carriage.rig.Pose(*ctx_.physics, &chassis_pos, chassis_rot)) {
    carriage.rig = world::CarriageRig{};
    return false;
  }
  const Quat chassis_q{chassis_rot[0], chassis_rot[1], chassis_rot[2], chassis_rot[3]};
  const Quat inverse = Conjugate(chassis_q);
  // The pieces come off their placements: the poses captured above become poses
  // on the chassis, and each gives up its static collision. Leaving no
  // PropPhysics behind is what keeps the streamer from putting a collider
  // straight back under a piece that is now moving.
  for (Part& part : parts) {
    part.local = Rotate(inverse, part.local - chassis_pos);
    part.local_rot = inverse * part.local_rot;
    const ecs::Entity e = EntityFor(part.ref);
    if (world::PropPhysics* body = ctx_.world->Get<world::PropPhysics>(e)) {
      if (body->body)
        ctx_.physics->RemoveBody(body->body);
      ctx_.world->Remove<world::PropPhysics>(e);
    }
  }
  carriage.parts = base::move(parts);

  carriage.driving = true;
  carriage.route = index;
  carriage.waypoint = 0;
  carriage.prev_hitch = HitchPoint(carriage);
  // The horse keeps its Seated tag: it is still in the shafts, and the ambient
  // sandbox must not wander it off mid-journey.
  const ecs::Entity horse = EntityFor(carriage.refs.horse);
  if (ctx_.world->IsAlive(horse)) {
    actors_->PlayNpcGait(horse, true);
    carriage.horse_seated = false;
    carriage.horse_walking = true;
  }
  RX_INFO("carriage: departing for {} ({} waypoints)", carriage.routes[index].destination,
          carriage.routes[index].waypoints.size() / 3);
  return true;
}

void CarriageSystem::Arrive(Carriage& carriage) {
  if (!carriage.driving)
    return;
  carriage.driving = false;
  if (ctx_.physics && carriage.rig.valid())
    ctx_.physics->RemoveVehicle(carriage.rig.vehicle());
  carriage.rig = world::CarriageRig{};
  // Hand the pieces back to the streamer where they now stand: PropPhysics with
  // no body is what makes it place a fresh static collider on the next sync.
  for (const Part& part : carriage.parts) {
    const ecs::Entity e = EntityFor(part.ref);
    if (ctx_.world->IsAlive(e) && !ctx_.world->Has<world::PropPhysics>(e))
      ctx_.world->Add(e, world::PropPhysics{0, world::PropMotion::kStatic});
  }
  carriage.parts.clear();
  carriage.horse_walking = false;
  carriage.horse_seated = false;  // Park re-plays the standing idle
  if (carriage.route < carriage.routes.size())
    RX_INFO("carriage: arrived at {}", carriage.routes[carriage.route].destination);
}

Vec3 CarriageSystem::HitchPoint(const Carriage& carriage) const {
  Vec3 horse_pos;
  Quat horse_rot;
  if (!RefPose(carriage.refs.horse, &horse_pos, &horse_rot))
    return {};
  Vec3 hitch = horse_pos - ForwardOf(YawOf(horse_rot)) * kHitchBehindHorse;
  // Hold the shaft level with the tongue so the pull has no vertical component.
  if (ctx_.physics && carriage.rig.valid())
    hitch.y = carriage.rig.TonguePoint(*ctx_.physics).y;
  return hitch;
}

void CarriageSystem::Drive(Carriage& carriage, f32 dt) {
  const world::CarriageRoute& route = carriage.routes[carriage.route];
  const ecs::Entity horse = EntityFor(carriage.refs.horse);
  world::Transform* horse_t = ctx_.world->Get<world::Transform>(horse);
  if (!horse_t) {
    Arrive(carriage);  // the horse streamed out from under us
    return;
  }
  Vec3 horse_pos{horse_t->position[0], horse_t->position[1], horse_t->position[2]};

  // Waypoint bookkeeping. The marker chain is coarse (a handful of marks between
  // two holds), so each leg is walked over the navmesh where the bubble around
  // the player covers it and straight over the ground where it does not, which
  // is most of a journey that crosses the map.
  const size_t stops = route.waypoints.size() / 3;
  while (carriage.waypoint < stops &&
         Length(Flatten(GameToEngine(route.waypoints.data() + carriage.waypoint * 3) -
                        horse_pos)) <= kMarkReached)
    ++carriage.waypoint;
  if (carriage.waypoint >= stops) {
    Arrive(carriage);
    return;
  }
  const Vec3 mark = GameToEngine(route.waypoints.data() + carriage.waypoint * 3);
  Vec3 step_to = mark;
  if (npc_) {
    // Follow the navmesh corridor only while it makes ground toward the mark.
    // Past the bubble around the player, which is most of a leg between holds,
    // it hands back corners at the edge of what it knows, and chasing those
    // walks the carriage back and forth instead of down the road.
    const Vec3 corner = npc_->PathFor(carriage.refs.horse, horse_pos, mark);
    const f32 remaining = Length(Flatten(mark - horse_pos));
    if (Length(Flatten(corner - horse_pos)) > 1.0f && Length(Flatten(mark - corner)) < remaining)
      step_to = corner;
  }
  const Vec3 to_step = Flatten(step_to - horse_pos);
  if (Length(to_step) < 1e-3f)
    return;
  const Vec3 heading = Normalize(to_step);
  horse_pos = horse_pos + heading * (kTrotSpeed * dt);
  f32 ground = horse_pos.y;
  if (ctx_.streamer && ctx_.streamer->GroundHeight(horse_pos.x, horse_pos.z, &ground))
    horse_pos.y = ground;
  const f32 horse_yaw = YawOfForward(heading);
  horse_t->position[0] = horse_pos.x;
  horse_t->position[1] = horse_pos.y;
  horse_t->position[2] = horse_pos.z;
  SetYaw(horse_t->rotation, horse_yaw);
  actors_->SetNpcGait(horse, kTrotSpeed, true, horse_yaw);

  // The hitch point swings when the horse turns on the spot, which as a raw
  // finite difference reads as tens of metres a second and slaps the cart
  // across the road. No horse pulls faster than a gallop, so cap it there.
  const Vec3 hitch = HitchPoint(carriage);
  Vec3 hitch_velocity = dt > 0 ? (hitch - carriage.prev_hitch) * (1.0f / dt) : Vec3{};
  if (const f32 speed = Length(hitch_velocity); speed > kGallopSpeed)
    hitch_velocity = hitch_velocity * (kGallopSpeed / speed);
  carriage.prev_hitch = hitch;
  carriage.rig.Step(*ctx_.physics, hitch, hitch_velocity, dt);

  // A line a minute on how the journey is going: which mark it is walking to and
  // how far off it still is. The ride is minutes long and crosses cells, so this
  // is what tells a log whether it is progressing or wedged.
  carriage.report -= dt;
  if (carriage.report <= 0) {
    carriage.report = 60.0f;
    RX_INFO("carriage: bound for {}, mark {}/{}, {:.0f} m to it", route.destination,
            carriage.waypoint + 1, stops, Length(Flatten(mark - horse_pos)));
  }
}

void CarriageSystem::Step(f32 dt) {
  if (!ctx_.world || !ctx_.quest_world || !ctx_.physics || !ctx_.physics->initialized())
    return;
#if RECREATION_HAS_NET
  if (ctx_.client_session)
    return;  // host authoritative, like the rest of NPC motion
#endif
  discover_timer_ -= dt;
  if (discover_timer_ <= 0) {
    discover_timer_ = 1.0f;
    Discover();
  }
  for (Carriage& carriage : carriages_) {
    if (!ctx_.world->IsAlive(EntityFor(carriage.refs.cart))) {
      if (carriage.driving)
        Arrive(carriage);  // its cell went out from under it
      continue;
    }
    if (carriage.driving)
      Drive(carriage, dt);
    else
      Park(carriage);
  }
  AutoRide();
}

void CarriageSystem::AutoRide() {
  const char* want = RideTo.get();
  if (!want || auto_ride_done_ || carriages_.empty())
    return;
  Carriage& carriage = carriages_.front();
  if (!ctx_.world->IsAlive(EntityFor(carriage.refs.horse)))
    return;  // let the whole rig stream in first
  if (riding_ < 0 && !Activate(carriage.refs.cart))
    return;
  bool offered = false;
  for (size_t i = 0; i < carriage.routes.size(); ++i) {
    if (want[0] && carriage.routes[i].destination.find(want) == base::String::npos)
      continue;
    offered = true;
    auto_ride_done_ = Depart(carriage, i);
    break;
  }
  if (!offered) {
    RX_WARN("carriage: no route from here to '{}'", want);
    auto_ride_done_ = true;
  }
}

void CarriageSystem::SyncRender() {
  if (!ctx_.physics || ctx_.config->headless)
    return;
  for (Carriage& carriage : carriages_) {
    if (!carriage.driving || !carriage.rig.valid())
      continue;
    Vec3 chassis_pos;
    f32 chassis_rot[4];
    if (!carriage.rig.Pose(*ctx_.physics, &chassis_pos, chassis_rot))
      continue;
    const Quat chassis_q{chassis_rot[0], chassis_rot[1], chassis_rot[2], chassis_rot[3]};
    for (const Part& part : carriage.parts) {
      const ecs::Entity e = EntityFor(part.ref);
      world::Transform* t = ctx_.world->Get<world::Transform>(e);
      if (!t)
        continue;
      const Vec3 p = chassis_pos + Rotate(chassis_q, part.local);
      const Quat r = chassis_q * part.local_rot;
      t->position[0] = p.x;
      t->position[1] = p.y;
      t->position[2] = p.z;
      t->rotation[0] = r.x;
      t->rotation[1] = r.y;
      t->rotation[2] = r.z;
      t->rotation[3] = r.w;
    }
    // The driver rides his bench, which is the cart piece itself.
    Vec3 cart, forward;
    Quat cart_rot;
    const ecs::Entity driver = EntityFor(carriage.refs.driver);
    if (ctx_.world->IsAlive(driver) && CartForward(carriage, &forward) &&
        RefPose(carriage.refs.cart, &cart, &cart_rot)) {
      if (world::Transform* t = ctx_.world->Get<world::Transform>(driver)) {
        t->position[0] = cart.x;
        t->position[1] = cart.y;
        t->position[2] = cart.z;
        SetYaw(t->rotation, YawOfForward(forward));
      }
    }
  }
}

bool CarriageSystem::SeatEye(const Carriage& carriage, Vec3* eye) const {
  Vec3 seat;
  Quat rotation;
  // The passenger bench, or the cart body while the bench is still streaming in.
  if (!RefPose(carriage.refs.seat, &seat, &rotation) &&
      !RefPose(carriage.refs.cart, &seat, &rotation))
    return false;
  *eye = seat + Vec3{0, kSeatEyeHeight, 0};
  return true;
}

base::Vector<base::String> CarriageSystem::RidePrompts() const {
  base::Vector<base::String> prompts;
  if (riding_ < 0)
    return prompts;
  const Carriage& carriage = carriages_[static_cast<size_t>(riding_)];
  if (carriage.driving) {
    prompts.push_back("Bound for " + carriage.routes[carriage.route].destination);
    prompts.push_back("[E]  Stop here");
    return prompts;
  }
  if (carriage.routes.empty()) {
    prompts.push_back("This carriage goes nowhere from here");
    prompts.push_back("[E]  Get off");
    return prompts;
  }
  prompts.push_back("Where to?");
  for (size_t i = 0; i < carriage.routes.size() && i < kMaxOfferedRoutes; ++i)
    prompts.push_back("[" + base::ToString(static_cast<int>(i) + 1) + "]  " +
                      carriage.routes[i].destination);
  prompts.push_back("[E]  Get off");
  return prompts;
}

bool CarriageSystem::Activate(u64 handle) {
  i32 index = -1;
  for (size_t i = 0; i < carriages_.size(); ++i) {
    const world::CarriageRefs& refs = carriages_[i].refs;
    if (handle == refs.cart || (refs.seat && handle == refs.seat))
      index = static_cast<i32>(i);
  }
  if (riding_ >= 0) {
    // Aboard: activating anything gets off, stopping the journey where it is.
    Carriage& carriage = carriages_[static_cast<size_t>(riding_)];
    if (carriage.driving)
      Arrive(carriage);
    riding_ = -1;
    ctx_.ride_active = false;
    ctx_.third_person = true;
    Vec3 eye;
    if (SeatEye(carriage, &eye) && actors_)
      actors_->TeleportPlayer(eye.x, eye.y - kSeatEyeHeight, eye.z);
    RX_INFO("carriage: left the carriage");
    return true;
  }
  if (index < 0)
    return false;

  Carriage& carriage = carriages_[static_cast<size_t>(index)];
  // Which holds this carriage serves is a property of where it stands, so the
  // routes are resolved the first time someone boards it.
  if (carriage.routes.empty() && ctx_.records) {
    Vec3 cart;
    Quat rotation;
    if (RefPose(carriage.refs.cart, &cart, &rotation)) {
      const f32 home[3] = {cart.x / kUnitsToMeters, -cart.z / kUnitsToMeters,
                           cart.y / kUnitsToMeters};
      carriage.routes = world::ResolveCarriageRoutes(*ctx_.records, carriage.refs.horse, home);
    }
    for (const world::CarriageRoute& route : carriage.routes)
      RX_INFO("carriage: serves {} ({} marks)", route.destination, route.waypoints.size() / 3);
  }
  riding_ = index;
  ctx_.ride_active = true;
  ctx_.third_person = false;  // the game forces first person for the ride
  ride_yaw_ = 0;  // facing over the horse; mouse look turns from there
  ride_pitch_ = 0;
  RX_INFO("carriage: boarded");
  return true;
}

const char* CarriageSystem::Label(u64 handle) const {
  for (const Carriage& carriage : carriages_)
    if (handle == carriage.refs.cart || (carriage.refs.seat && handle == carriage.refs.seat))
      return label_.c_str();
  return nullptr;
}

void CarriageSystem::UpdateRide(f32 dt, const InputState& input, const ActionState& actions) {
  (void)dt;
  (void)actions;
  if (riding_ < 0)
    return;
  Carriage& carriage = carriages_[static_cast<size_t>(riding_)];

  if (!carriage.driving) {
    const Key digits[kMaxOfferedRoutes] = {Key::k1, Key::k2, Key::k3,
                                           Key::k4, Key::k5, Key::k6};
    for (size_t i = 0; i < carriage.routes.size() && i < kMaxOfferedRoutes; ++i) {
      if (input.key_pressed(digits[i])) {
        Depart(carriage, i);
        break;
      }
    }
  }

  // Look around from the seat. The heading is held relative to the cart, so a
  // passenger keeps facing over the horse as the carriage turns instead of
  // being left staring at the roadside.
  ride_yaw_ -= input.mouse_dx * kLookSensitivity;
  ride_pitch_ = base::Clamp(ride_pitch_ - input.mouse_dy * kLookSensitivity, -1.4f, 1.4f);
  Vec3 eye, forward;
  if (!SeatEye(carriage, &eye) || !CartForward(carriage, &forward))
    return;
  const f32 yaw = YawOfForward(forward) + ride_yaw_;
  const f32 cos_pitch = std::cos(ride_pitch_);
  const Vec3 look{-std::sin(yaw) * cos_pitch, std::sin(ride_pitch_), -std::cos(yaw) * cos_pitch};
  ctx_.walk_eye = eye;
  ctx_.walk_target = eye + look;
  ctx_.cam_yaw = yaw;
  if (actors_)
    actors_->TeleportPlayer(eye.x, eye.y - kSeatEyeHeight, eye.z);
  if (ctx_.camera) {
    ctx_.camera->set_position(eye);
    ctx_.camera->set_yaw_pitch(yaw, ride_pitch_);
  }
}

}  // namespace rx

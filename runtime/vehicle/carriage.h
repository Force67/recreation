#ifndef RECREATION_RUNTIME_VEHICLE_CARRIAGE_H_
#define RECREATION_RUNTIME_VEHICLE_CARRIAGE_H_

#include <base/containers/unordered_set.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>
#include <mutex>

#include "components/bethesda/form_id.h"
#include "components/world/carriage_records.h"
#include "components/world/carriage_rig.h"
#include "components/world/road_map.h"
#include "core/math.h"
#include "ecs/world.h"

namespace rx {

struct EngineContext;
struct InputState;
struct ActionState;
class ActorSystem;
class NpcDirector;

// The hold carriages, run off the game's own records.
//
// A carriage is a driver, a horse and three furniture pieces linked together
// (world::CarriageRefs). Parked, the horse is put on its harness mark, which is
// what hitches it in the shafts, and the driver takes his bench with the game's
// driver idle. Board one and pick a hold, and the horse is sent along that
// journey's authored marker chain while the cart comes off its placement and
// rolls behind it on a real physics rig (world::CarriageRig), pulled through a
// spring-damper hitch. That is what CarriageSystemScript does: the cart goes
// from keyframed to dynamic and the horse is handed a CartHorsePatrol package.
//
// Host authoritative, like the rest of NPC motion.
class CarriageSystem {
 public:
  CarriageSystem(EngineContext& ctx, ActorSystem* actors, NpcDirector* npc);

  // Fixed step, staged BEFORE physics (kPreSim, the rx vehicle update-ordering
  // contract): finds carriages in the streamed cells, holds parked ones
  // together, and advances a journey's horse plus the tow force on its cart.
  void Step(f32 dt);
  // After physics (kPostSim): the cart's pieces and its riders follow the
  // chassis the solver just moved.
  void SyncRender();

  bool riding() const { return riding_ >= 0; }
  // Frame-cadence ride update: seats the player, reads the destination keys and
  // frames the view from the seat (first person, as the game forces).
  void UpdateRide(f32 dt, const InputState& input, const ActionState& actions);

  // The cart racing kit: commands the ridden cart `cart` to steer [-1,1] and
  // throttle [0,1], overriding the journey's path-follow while that ride lasts.
  // Called on the guest thread (from the script bindings' Vehicle.Drive); the
  // values cross to the main thread through a mutex slot drained by Drive.
  void DriveRemote(f32 steer, f32 throttle);
  // The ridden cart's forward speed in m/s from its physics body, and whether
  // the player is riding at all. False/0 when nothing is ridden or the rig is
  // down. Main thread only.
  bool RiddenCartSpeed(f32* speed) const;
  // Snaps the ridden ride back to a game-space position (respawn), guest thread.
  void MoveRemote(f32 x, f32 y, f32 z);
  // kActivateRef handler: boards or leaves the carriage `handle` belongs to.
  // Returns true when it owned the handle.
  bool Activate(u64 handle);
  // Activation prompt for a carriage piece, or null when `handle` is not one.
  const char* Label(u64 handle) const;
  // What a seated passenger is offered: the destination list, or where the ride
  // is bound. Empty on foot. The frame loop folds these into the HUD prompts.
  base::Vector<base::String> RidePrompts() const;

 private:
  // A part of the cart that rides the chassis: its pose in chassis space,
  // captured when the journey starts so the rig keeps the authored shape.
  struct Part {
    u64 ref = 0;
    Vec3 local{};
    Quat local_rot{0, 0, 0, 1};
  };

  struct Carriage {
    world::CarriageRefs refs;
    base::Vector<world::CarriageRoute> routes;

    // Journey state. `driving` owns the physics rig; parked, the pieces keep
    // their placement and their static collision.
    bool driving = false;
    size_t route = 0;
    size_t waypoint = 0;
    world::CarriageRig rig;
    base::Vector<Part> parts;
    // Wheels lifted out of the cart's art, each drawn at the physics wheel it
    // sits on so it rolls and takes the suspension.
    struct Wheel {
      ecs::Entity entity{};
      u32 index = 0;  // which of the vehicle's wheels this one is
    };
    base::Vector<Wheel> wheels;
    bool wheels_split = false;  // the art has been cut up (once per carriage)
    Vec3 prev_hitch{};
    // The road worked out for the leg being driven, and how far along it the
    // carriage is. Empty where no road was found; the mark is then driven at
    // directly.
    base::Vector<Vec3> leg;
    size_t leg_next = 0;
    f32 replan = 0;
    f32 report = 0;  // throttles the journey's progress log
    f64 road_total = 0;
    f64 road_samples = 0;
    bool horse_walking = false;
    // The seated idles only take once the actor has a drawable instance, which
    // lands a frame or two after its cell does.
    bool driver_seated = false;
    bool horse_seated = false;
    // Player-drive override (the cart racing kit): when set, Drive steers with
    // the commanded input instead of following the marker chain. Set by
    // DriveRemote, cleared by Arrive (the ride ending), so it cannot leak into
    // ordinary journeys.
    bool player_driven = false;
    f32 drive_steer = 0;
    f32 drive_throttle = 0;
  };

  // Adds any carriage whose driver has streamed in and is not known yet.
  void Discover();
  // RX_CARRIAGE_RIDE: boards the first carriage to stream in and sets off.
  void AutoRide();
  // Holds a parked carriage together: the horse on its harness mark, the driver
  // on his bench.
  void Park(Carriage& carriage);
  // Adds the carriage art that carries no link of its own (the wagon body is a
  // plain static beside the furniture) to the pieces the chassis will carry.
  void AddLooseArt(const Carriage& carriage, const Vec3& cart, base::Vector<Part>* parts);
  base::String ModelPathOfBase(bethesda::GlobalFormId base) const;
  base::String ModelPathOfRef(u64 ref) const;
  // Cuts the wheels out of whichever piece of art carries them, leaving that
  // piece the body and giving each wheel its own entity. Fills in the hub
  // geometry the vehicle should be built with (track, wheelbase, radius and the
  // height the axles want to sit at). False when the art has no wheels in it, in
  // which case the cart still rolls, it just does not show it.
  bool SplitWheels(Carriage& carriage,
                   const base::Vector<Part>& parts,
                   const Vec3& forward,
                   const Vec3& centre,
                   world::CarriageConfig* cfg,
                   f32* axle_height);
  // Pairs each art wheel with the vehicle wheel nearest it, so the art follows
  // the physics whatever order either of them is in.
  void BindWheelsToVehicle(Carriage& carriage);
  // Sends `carriage` down route `index`: the cart's pieces come off their
  // placements onto a physics chassis and the horse starts walking.
  bool Depart(Carriage& carriage, size_t index);
  // Ends a journey where the carriage now stands, handing the pieces back to
  // the streamer as placed (and collidable) objects.
  void Arrive(Carriage& carriage);
  // One journey step: waypoint bookkeeping plus the tow force on the cart.
  void Drive(Carriage& carriage, f32 dt);

  // How much road is under a world position, decoding the cell's landscape the
  // first time it is asked about. 0 off the road and where there is no land.
  f32 RoadAt(const Vec3& position);
  bool EnsureRoadSource();
  // Works out the road between where the carriage stands and the next mark, and
  // hands the journey the corners to drive.
  void PlanLeg(Carriage& carriage, const Vec3& from, const Vec3& mark);

  ecs::Entity EntityFor(u64 ref) const;
  bool RefPose(u64 ref, Vec3* position, Quat* rotation) const;
  // Ground-plane direction from the cart toward the horse's mark: the axis the
  // whole rig is built on, taken from the placements rather than assumed.
  bool CartForward(const Carriage& carriage, Vec3* forward) const;
  Vec3 HitchPoint(const Carriage& carriage) const;
  // Where a passenger's eyes are: the seat piece, at head height.
  bool SeatEye(const Carriage& carriage, Vec3* eye) const;

  EngineContext& ctx_;
  ActorSystem* actors_;
  NpcDirector* npc_;
  world::CarriageKeywords keywords_;
  bool keywords_resolved_ = false;
  // The roads the landscape is painted with, decoded cell by cell as a journey
  // reaches them, plus the cells already looked at (a cell with no land, or none
  // painted, must not be re-parsed every step).
  world::RoadMap roads_;
  base::UnorderedSet<u32> road_cells_read_;
  bethesda::GlobalFormId worldspace_{};
  bool worldspace_resolved_ = false;
  base::Vector<Carriage> carriages_;
  base::UnorderedSet<u64> examined_;  // actors already checked for a carriage
  f32 discover_timer_ = 0;

  bool auto_ride_done_ = false;
  i32 riding_ = -1;  // index into carriages_, -1 = on foot
  f32 ride_yaw_ = 0;  // passenger's heading, relative to the way the cart points
  f32 ride_pitch_ = 0;
  base::String label_;
  // The remote-drive command slot, written on the guest thread by DriveRemote
  // and drained by the ride's Drive on the main thread. Applies to the ridden
  // cart only.
  struct RemoteDrive {
    std::mutex mutex;
    f32 steer = 0;
    f32 throttle = 0;
    bool armed = false;
  };
  RemoteDrive remote_drive_;
  // Remote-respawn request, written on the guest thread by MoveRemote and
  // drained by Step.
  struct RemoteMove {
    std::mutex mutex;
    float x = 0;
    float y = 0;
    float z = 0;
    bool armed = false;
  };
  RemoteMove remote_move_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_VEHICLE_CARRIAGE_H_

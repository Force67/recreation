#ifndef RECREATION_RUNTIME_NARRATIVE_HELGEN_INTRO_H_
#define RECREATION_RUNTIME_NARRATIVE_HELGEN_INTRO_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "core/math.h"
#include "components/quest/package_record.h"
#include "ecs/world.h"
#include "runtime/demo/trailer.h"

namespace rx {

struct EngineContext;
class ActorSystem;

// Skyrim's opening: the prisoner cart ride down the mountain road into Helgen
// (RX_HELGEN_INTRO=1). Everything on screen comes out of the game's own data --
// the route is the Tamriel road the vanilla carts roll down, the cart and the
// horse are the game's mesh and creature rig, the passengers are the MQ101 NPCs
// with their FaceGen heads playing the authored cart-prisoner idle clips, and
// the dialogue is read phase by phase out of the MQ101 SCEN records rather than
// transcribed into this file.
//
// The cart is scripted, not simulated. Vanilla's is an animated reference too,
// and a kinematic cart riding the terrain heightfield arrives on its dialogue
// cue every run, which a spring-towed physics rig crossing 200 m of streaming
// terrain does not.
class HelgenIntro {
 public:
  HelgenIntro(EngineContext& ctx, ActorSystem* actors);

  bool enabled() const { return enabled_; }

  // Where the ride begins, for content load: the exterior cell streaming should
  // boot in, and the engine-space view looking down the road. Both resolve the
  // route out of the quest records, so they need those loaded; false when the
  // cutscene is off or the quest is absent, leaving the normal start alone.
  bool StartCell(i32* cell_x, i32* cell_y);
  bool StartView(Vec3* eye, Vec3* target);

  // Per-frame, from the camera update. True while the cutscene owns the camera
  // and the player's input; false when it is off or has finished.
  bool Update(f32 dt);

  // Cinematic chrome (letterbox, fades, the spoken line) for this frame.
  const TrailerOverlay& overlay() const { return overlay_; }

 private:
  enum class Stage : u8 { kWaitForWorld, kRide, kArrived, kDone };

  // One spoken beat, in the order its scene phase plays it.
  struct Line {
    i32 phase = 0;
    base::String speaker;  // the MQ101 alias name, which is how the scene casts it
    base::String text;
    f32 start = 0;  // cutscene seconds
    f32 end = 0;
  };

  // Somebody riding in the cart; their clip seats them.
  struct Passenger {
    ecs::Entity entity;
    base::String alias;  // matched against a Line's speaker for the camera
  };

  // Resolves the route from the quest alias's travel packages. Cached; false
  // until the records are loaded.
  bool BuildRoute();
  // Reads the MQ101 cart scenes into `lines_`, ordered by scene phase and paced
  // by how long each line takes to read.
  void LoadDialogue();
  // Picks the package the horse should be running from live quest state, and
  // returns how far along the route that lets it travel. Falls back to the whole
  // route when the quest is not running the ride.
  f32 PackageLimit();
  bool WorldReady() const;
  void Spawn();
  void Advance(f32 dt);
  void PlaceCart();
  void DriveCamera();
  void UpdateOverlay();

  // Route point and unit travel direction at `arc` metres along the road.
  Vec3 RouteSample(f32 arc, Vec3* forward) const;
  // Terrain height under an engine-space x/z, `fallback` where none is loaded.
  f32 GroundY(f32 x, f32 z, f32 fallback) const;
  // The NPC_ base a reference alias fills with, via its forced reference or
  // unique-actor rule. Zero when the alias is absent or fills some other way.
  bethesda::GlobalFormId AliasActor(const base::String& name) const;
  // The NPC_ base behind a placed actor reference, by its editor id.
  bethesda::GlobalFormId RefBase(const base::String& editor_id) const;
  // A point in the cart's local space, in the world, this frame.
  Vec3 CartLocal(const Vec3& offset) const;
  // World position of a rider's head, by MQ101 alias. False if they are absent.
  bool RiderHead(const base::String& alias, Vec3* out) const;
  // A point down the road ahead of the cart, at eye height.
  Vec3 RoadAhead() const;

  EngineContext& ctx_;
  ActorSystem* actors_;
  bool enabled_ = false;
  Stage stage_ = Stage::kWaitForWorld;
  f32 time_ = 0;         // seconds since the ride started
  f32 wait_time_ = 0;    // seconds spent waiting for the world to stream in
  f32 arrive_time_ = 0;  // cutscene time the cart reached the square
  f32 arc_ = 0;          // metres travelled along the road
  f32 speed_ = 2.2f;     // cart pace, set so the ride lands on the closing lines
  f32 route_length_ = 0;

  quest::QuestDef quest_;  // the owning quest, for aliases and packages
  u16 quest_plugin_ = 0;   // its plugin, to resolve the raw ids it carries
  u64 quest_handle_ = 0;   // packed form id, for stage queries

  // The horse's AI packages in record order (highest priority first), which is
  // the order SelectActivePackage expects. `arc` is how far along the route that
  // package's destination sits, and `stage` the journal stage its conditions
  // gate on -- reaching a leg advances the quest to the next leg's stage, which
  // is what the package's own fragment does in the game.
  struct Leg {
    quest::PackageDef def;
    f32 arc = 0;
    i32 stage = -1;
  };
  base::Vector<Leg> legs_;
  int active_leg_ = -1;
  i32 advanced_stage_ = -1;      // last stage this drove the quest to, so it fires once
  base::Vector<Vec3> route_;     // engine-space route, from the alias packages
  base::Vector<f32> route_arc_;  // cumulative arc length at each waypoint
  base::Vector<Line> lines_;
  size_t line_ = 0;  // index of the line currently on screen

  // Live cart pose, rebuilt each frame from the route and the terrain.
  Vec3 cart_pos_{};
  Quat cart_rot_{0, 0, 0, 1};

  ecs::Entity cart_entity_{};
  ecs::Entity horse_entity_{};
  base::Vector<Passenger> riders_;
  Vec3 cam_target_{};
  bool cam_target_valid_ = false;

  TrailerOverlay overlay_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_NARRATIVE_HELGEN_INTRO_H_

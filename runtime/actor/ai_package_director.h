#ifndef RECREATION_RUNTIME_ACTOR_AI_PACKAGE_DIRECTOR_H_
#define RECREATION_RUNTIME_ACTOR_AI_PACKAGE_DIRECTOR_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "components/bethesda/script_attachment.h"
#include "components/quest/package_record.h"
#include "components/quest/quest_def.h"
#include "core/math.h"
#include "runtime/app/engine_context.h"
#include "runtime/narrative/quest_state_cache.h"

namespace rx {

class ActorSystem;
class NpcDirector;

// Runs Bethesda's AI packages, which is how the games move actors through a
// scripted sequence. Nothing about it is scripted per quest: a quest stacks
// packages on its aliases (the ALPC list, highest priority first), each gated by
// conditions that are almost always the journal stage, and the actor runs the
// first package whose gate passes. A travel package walks it to a marker and, on
// arrival, runs the package's own Papyrus fragment, which sets the stage that lets
// the next package take over.
//
// Skyrim's opening cart ride is exactly that and nothing else: five travel
// packages on the cart horse's alias, a cart towed by its enable parent, and
// passengers held in place by a no-travel package. So is the escort out of Helgen,
// and so is every scene where actors walk to their marks before speaking.
class AiPackageDirector {
 public:
  AiPackageDirector(EngineContext& ctx, ActorSystem* actors, NpcDirector* npc);

  // Indexes one quest's alias packages and the attachments its aliases describe
  // (a placed object whose enable parent is one of the actors gets towed by it).
  // Records only, so it is safe to call while the quest definitions are parsed.
  void ArmQuest(u64 quest, u16 plugin, const quest::QuestDef& def);

  // Selects and drives packages. `quests` is the main-thread quest mirror the
  // package gates are evaluated against.
  void Tick(f32 dt, const QuestStateCache& quests);

  // A scene action handing a package to its performer, outside the alias stack:
  // it takes priority until the scene closes the action's phase window.
  void RunScenePackage(u64 actor, u64 package, u64 quest, u16 plugin);
  void StopScenePackage(u64 actor, u64 package);

  // Where a quest's scripted journey begins: the placement of the actor pulling
  // the vehicle it travels on, else of the first alias actor it hands a travel
  // package to. The cutscene director boots the world there so a ride starts with
  // its route streamed in. False when the quest travels nowhere.
  bool JourneyStart(u64 quest, Vec3* pos) const;

  int armed_count() const { return static_cast<int>(slots_.size()); }
  int travelling_count() const;
  // One line per armed alias for the debug panel: who is running what, and how far
  // they still have to walk.
  base::Vector<base::String> Report() const;

 private:
  // One package on an alias's stack, with the fragments that fire around it.
  struct Package {
    quest::PackageDef def;
    base::String editor_id;
    bethesda::PackageFragments frags;
    bethesda::ScriptAttachment scripts;
    bool has_scripts = false;
    bool attached = false;
  };

  // One alias and the packages it stacks on its actor.
  struct Slot {
    u64 quest = 0;
    u16 plugin = 0;
    i32 alias = -1;
    base::String name;
    u64 actor = 0;  // the placed reference the alias fills, which is the form handle
    base::Vector<Package> packages;
    int active = -1;  // index into `packages`, -1 when no gate passes
    int forced = -1;  // a scene-assigned package, which outranks the stack
    u64 forced_handle = 0;
    bool has_dest = false;
    bool arrived = false;
    bool fired = false;  // the arrival fragment has run for this leg
    Vec3 dest{};
    u64 dest_ref = 0;  // the mark `dest` came from, so its chain can be followed
    int chain = 0;     // marks walked on this leg, to stop a looped chain
    f32 radius = 2.0f;
    f32 stall = 0;      // seconds without meaningful progress
    f32 last_dist = 0;  // distance to the goal when progress was last measured
  };

  // A placed object towed by the actor it is enable-parented to (the cart behind
  // the horse). The offset comes from the authored placements, so the rig holds the
  // shape the level designer built.
  struct Tow {
    u64 ref = 0;
    u64 parent = 0;
    Vec3 local{};  // offset in the parent's authored frame, metres
    f32 local_yaw = 0;
    bool cart = false;  // its model is a cart, so it has seated idles for its riders
    int seats = 0;      // riders taken on so far, which picks each one's idle
  };

  // An actor riding a towed object: it keeps its authored place on the ride and
  // plays the ride's seated idle instead of walking.
  // Lets a rider off: the ride is gone, or something outside the director (an
  // end-of-ride quest teleport) has moved them clear of it. Without this
  // SeatRiders would keep pinning them to the vehicle every tick and no script
  // could ever get them out.
  void ReleaseFinishedRiders();

  struct Rider {
    u64 actor = 0;
    u64 ride = 0;
    Vec3 local{};
    f32 local_yaw = 0;
    base::String clip;
    bool seated = false;
  };

  void SelectPackages(const QuestStateCache& quests);
  void DriveTravel(f32 dt);
  void DriveTows();
  void SeatRiders();
  // Takes on anyone standing in a moving object's footprint as a rider. Quests put
  // their cast aboard at runtime (Skyrim's intro teleports the prisoners into the
  // cart), so the rig cannot be settled from the authored placements alone.
  void CaptureRiders(Tow& tow, const Vec3& ride_pos, f32 ride_yaw);
  bool ResolveDestination(const Slot& slot,
                          const Package& pack,
                          Vec3* out,
                          f32* radius,
                          u64* ref) const;
  // The next mark a patrol's route chains on to (the unkeyworded XLKR off a
  // marker), or 0 where the route ends.
  u64 MarkerChainNext(u64 marker) const;
  // Runs a package fragment on the guest thread (attaching its PF_ script first).
  void FirePackageFragment(Slot& slot, Package& pack, const base::String& function);
  bool ActorPose(u64 handle, Vec3* pos, f32* yaw) const;
  bool RefRecordPose(bethesda::GlobalFormId ref, Vec3* pos, f32* yaw) const;
  // The reference an alias fills, resolved from the records (a forced reference, or
  // the unique actor's own placement). 0 when the alias fills some other way.
  u64 AliasReference(const quest::AliasDef& alias, u16 plugin) const;

  EngineContext& ctx_;
  ActorSystem* actors_;
  NpcDirector* npc_;
  base::Vector<Slot> slots_;
  base::Vector<Tow> tows_;
  base::Vector<Rider> riders_;
  f32 select_timer_ = 0;
  f32 rider_timer_ = 0;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_ACTOR_AI_PACKAGE_DIRECTOR_H_

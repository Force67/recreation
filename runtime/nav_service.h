#ifndef RECREATION_RUNTIME_NAV_SERVICE_H_
#define RECREATION_RUNTIME_NAV_SERVICE_H_

// Exterior NPC navigation over rx::nav: a cost-aware navmesh sampled from the
// streamed terrain (LAND heights + water table) inside a bubble around the
// player, plus per-actor corridors with event-based repathing.
//
// Terrain desirability, Death Stranding style: steep ground and water stay
// traversable but cost. Slopes an NPC could stumble down carry a traversal
// multiplier, rivers a multiplier plus a one-time entry toll -- so pursuit
// paths prefer smooth ground, ford water at narrow spots, and an NPC that DID
// step into a river commits to the crossing instead of turning 360 back out.
//
// Interiors (no streamed LAND) fall out naturally: the sampler reports no
// surface, tiles stay empty, Covers() returns false and the director keeps
// using its raycast navgrid there.

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/math.h"
#include "nav/navmesh.h"
#include "nav/path.h"

namespace rx {

struct EngineContext;

namespace world {
class CellStreamer;
}

// Areas painted by the terrain sampler (rx::nav ids; 1 = plain ground).
inline constexpr nav::AreaId kNavAreaRough = 2;   // stumble-grade slopes
inline constexpr nav::AreaId kNavAreaWater = 3;   // wadeable water
inline constexpr nav::AreaId kNavAreaDeep = 4;    // swimming depth

// Corridor identity of the walking player (form handles start well above it).
inline constexpr u64 kPlayerNavId = 1;

class NavService {
 public:
  explicit NavService(EngineContext& ctx);

  // Per-tick bubble maintenance around the player: builds a few missing tiles
  // (time-sliced), retries tiles that were sampled before their cell streamed
  // in, drops tiles far behind, and resets the frame's repath budget.
  void Update(const Vec3& focus, f32 dt);

  // True when a walkable navmesh cell exists at (or right next to) pos --
  // i.e. this position is on streamed exterior terrain the service covers.
  bool Covers(const Vec3& pos) const;

  // The per-frame corridor step for actor `id`: validates the corridor
  // (event-based -- tile changed, goal cell moved, agent drifted off, partial
  // path exhausted), replans within the frame budget when a reason fired, and
  // returns the funnel's next corner from `from`. Falls back to `goal` when
  // the mesh cannot help (agent off coverage, no path yet).
  Vec3 Step(u64 id, const Vec3& from, const Vec3& goal);

  // Drops the corridor state of a despawned/dead actor.
  void Forget(u64 id);

  const nav::NavMesh& mesh() const { return mesh_; }

  // Corridor visitor for the debug overlay: fn(id, corridor).
  template <typename Fn>
  void ForEachCorridor(Fn&& fn) const {
    agents_.ForEach([&](const u64& id, const Agent& agent) { fn(id, agent.corridor); });
  }

 private:
  struct Agent {
    nav::Corridor corridor;
    f32 idle = 0;  // seconds since the last Step; pruned when stale
  };

  bool SampleTerrain(f32 x, f32 z, nav::Sample& out) const;
  void BuildAround(const Vec3& focus);

  EngineContext& ctx_;
  nav::NavMesh mesh_;
  nav::PathScratch scratch_;
  base::UnorderedMap<u64, Agent> agents_;
  // Tiles whose build found no surface (cell not streamed yet, or interior):
  // rebuilt on a cooldown so terrain appearing later fills the hole.
  struct EmptyTile {
    i32 tx, tz;
    f32 cooldown;
  };
  base::Vector<EmptyTile> empty_retry_;
  u32 repath_budget_ = 0;
  f32 prune_timer_ = 0;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_NAV_SERVICE_H_

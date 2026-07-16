#include "nav_service.h"

#include <algorithm>
#include <cmath>

#include "engine_context.h"
#include "world/cell_streaming.h"

namespace rx {
namespace {

constexpr f32 kBubbleRadius = 36.0f;   // meters of navmesh kept around the player
constexpr f32 kDropRadius = 90.0f;     // tiles farther than this are released
constexpr u32 kTilesPerTick = 3;       // time-sliced builds per Update
constexpr u32 kRepathsPerTick = 4;     // corridor replans shared by all actors
constexpr f32 kEmptyRetrySeconds = 1.5f;  // recheck holes (cell streamed in late)
constexpr f32 kAgentIdleSeconds = 6.0f;   // corridor state dropped after this

}  // namespace

NavService::NavService(EngineContext& ctx)
    : ctx_(ctx), mesh_(nav::NavMeshConfig{.cell_size = 0.5f, .tile_cells = 16}) {
  // Terrain economics: stumble-grade slopes cost extra to walk; water costs
  // more and charges a one-time entry toll (prefer fords, but commit once
  // wet); swimming depth is a last resort, never a wall.
  mesh_.SetAreaCost(kNavAreaRough, 1.7f, 1.0f);
  mesh_.SetAreaCost(kNavAreaWater, 2.5f, 2.5f);
  mesh_.SetAreaCost(kNavAreaDeep, 5.0f, 4.0f);
}

bool NavService::SampleTerrain(f32 x, f32 z, nav::Sample& out) const {
  world::CellStreamer* streamer = ctx_.streamer;
  if (!streamer) return false;
  f32 height = 0;
  if (!streamer->GroundHeight(x, z, &height)) return false;

  // Slope from the streamed heightmap. Missing neighbors (cell edge mid
  // stream) fall back to the center height: flat, revisited by the retry pass.
  auto probe = [&](f32 px, f32 pz) {
    f32 h = height;
    streamer->GroundHeight(px, pz, &h);
    return h;
  };
  const f32 gx = (probe(x + 0.4f, z) - probe(x - 0.4f, z)) / 0.8f;
  const f32 gz = (probe(x, z + 0.4f) - probe(x, z - 0.4f)) / 0.8f;
  const f32 slope_sq = gx * gx + gz * gz;
  if (slope_sq > 1.2f * 1.2f) return false;  // cliff: not standable

  out.height = height;
  out.area = slope_sq > 0.55f * 0.55f ? kNavAreaRough : nav::kAreaGround;

  f32 water = 0;
  if (streamer->WaterHeightAt(Vec3{x, height, z}, &water, nullptr)) {
    const f32 depth = water - height;
    if (depth > 1.5f) {
      out.area = kNavAreaDeep;
    } else if (depth > 0.2f) {
      out.area = kNavAreaWater;
    }
  }
  return true;
}

void NavService::BuildAround(const Vec3& focus) {
  const f32 tile_m = mesh_.config().cell_size * static_cast<f32>(mesh_.config().tile_cells);
  const i32 span = static_cast<i32>(std::ceil(kBubbleRadius / tile_m));
  const i32 fx = static_cast<i32>(std::floor(focus.x / tile_m));
  const i32 fz = static_cast<i32>(std::floor(focus.z / tile_m));

  struct Want {
    i32 tx, tz;
    f32 dist2;
  };
  base::Vector<Want> missing;
  for (i32 tz = fz - span; tz <= fz + span; ++tz) {
    for (i32 tx = fx - span; tx <= fx + span; ++tx) {
      if (mesh_.TileVersionAt(tx, tz) != 0) continue;
      const f32 cx = (static_cast<f32>(tx) + 0.5f) * tile_m - focus.x;
      const f32 cz = (static_cast<f32>(tz) + 0.5f) * tile_m - focus.z;
      const f32 d2 = cx * cx + cz * cz;
      if (d2 > kBubbleRadius * kBubbleRadius) continue;
      missing.push_back({tx, tz, d2});
    }
  }
  std::sort(missing.begin(), missing.end(),
            [](const Want& a, const Want& b) { return a.dist2 < b.dist2; });

  nav::SampleFn sampler = [this](f32 x, f32 z, nav::Sample& out) {
    return SampleTerrain(x, z, out);
  };
  u32 built = 0;
  for (const Want& want : missing) {
    if (built >= kTilesPerTick) break;
    ++built;
    if (!mesh_.BuildTile(want.tx, want.tz, sampler)) {
      // Nothing standable: either an interior or the cell has not streamed
      // in yet. Rebuild on a cooldown so late terrain fills the hole.
      empty_retry_.push_back({want.tx, want.tz, kEmptyRetrySeconds});
    }
  }
}

void NavService::Update(const Vec3& focus, f32 dt) {
  repath_budget_ = kRepathsPerTick;

  // Empty-tile retries: an expired entry resamples its tile in place (the
  // empty tile blocks BuildAround from picking it up again). Still-empty
  // tiles re-arm; one rebuild per tick keeps the cost flat.
  nav::SampleFn sampler = [this](f32 x, f32 z, nav::Sample& out) {
    return SampleTerrain(x, z, out);
  };
  i32 rebuild = -1;
  for (u32 i = 0; i < empty_retry_.size(); ++i) {
    empty_retry_[i].cooldown -= dt;
    if (rebuild < 0 && empty_retry_[i].cooldown <= 0) rebuild = static_cast<i32>(i);
  }
  if (rebuild >= 0) {  // at most one rebuild per tick keeps the cost flat
    const EmptyTile retry = empty_retry_[rebuild];
    if (mesh_.BuildTile(retry.tx, retry.tz, sampler)) {
      empty_retry_.erase(static_cast<u32>(rebuild));
    } else {
      empty_retry_[rebuild].cooldown = kEmptyRetrySeconds;
    }
  }

  BuildAround(focus);

  prune_timer_ += dt;
  if (prune_timer_ >= 2.0f) {
    prune_timer_ = 0;
    mesh_.RemoveTilesBeyond(focus, kDropRadius);
    // Corridor state of actors that stopped navigating.
    base::Vector<u64> stale;
    agents_.ForEach([&](const u64& id, Agent& agent) {
      agent.idle += 2.0f;
      if (agent.idle > kAgentIdleSeconds) stale.push_back(id);
    });
    for (u64 id : stale) agents_.erase(id);
  }
}

bool NavService::Covers(const Vec3& pos) const {
  return mesh_.ClampToWalkable(pos, 1.2f).valid();
}

Vec3 NavService::Step(u64 id, const Vec3& from, const Vec3& goal) {
  Agent& agent = agents_[id];
  agent.idle = 0;

  // Event-based repathing: replan only when validation names a reason, and
  // only within this frame's shared budget. A denied replan keeps steering
  // along the stale corridor -- incremental progress beats standing still.
  const nav::RepathReason reason = nav::ValidateCorridor(mesh_, &agent.corridor, from, goal);
  if (reason != nav::RepathReason::kNone && repath_budget_ > 0) {
    --repath_budget_;
    nav::PathRequest request;
    request.start = from;
    request.goal = goal;
    request.max_iterations = 500;  // partial paths extend as the agent walks
    request.clamp_radius = 4.0f;
    nav::FindPath(mesh_, request, scratch_, &agent.corridor);
  }
  if (!agent.corridor.valid()) return goal;

  Vec3 corner;
  if (!nav::NextCorner(mesh_, &agent.corridor, from, 0.3f, &corner)) return goal;
  return corner;
}

void NavService::Forget(u64 id) { agents_.erase(id); }

}  // namespace rx

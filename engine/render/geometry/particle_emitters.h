#ifndef RECREATION_RENDER_PARTICLE_EMITTERS_H_
#define RECREATION_RENDER_PARTICLE_EMITTERS_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "core/math.h"
#include "render/geometry/particles.h"

namespace rec::render {

// CPU simulation of the emitters parsed from NIF particle systems
// (asset::Mesh::emitters). Every placed instance of an emitting mesh gets its
// own particle pool, keyed by mesh + world position (draw items carry no
// stable instance id, but placed refs do not move). Pools are stepped once
// per frame and appended to the billboard sets the ParticleSystem pass draws.
// Pure CPU state, no GPU resources to destroy.
class ParticleEmitterSim {
 public:
  // Starts a frame: dt advances the pools, emitters beyond max_distance of
  // `camera` are not spawned, pools untouched for a while are dropped.
  void BeginFrame(f32 dt, const Vec3& camera, f32 max_distance = 60.0f);

  // Registers one placed instance of a mesh's emitters this frame
  // (transform maps mesh space to engine world space, scale included).
  void AddInstance(u64 mesh_key, const base::Vector<asset::ParticleEmitter>& emitters,
                   const Mat4& transform);

  // Steps every touched pool and appends billboards: `lit` gets the
  // alpha-blended smoke/mist, `additive` the HDR fire.
  void Simulate(base::Vector<ParticleInstance>* lit, base::Vector<ParticleInstance>* additive);

  u32 live_particles() const { return live_; }

 private:
  struct Particle {
    Vec3 pos{};
    Vec3 prev{};
    Vec3 vel{};
    f32 age = 0;
    f32 life = 1;
    f32 size = 0.1f;
  };
  // One emitter of one placed instance, parameters baked into engine world.
  struct Pool {
    Vec3 origin{};
    Vec3 velocity{};
    Vec3 extent{};
    Vec3 gravity{};
    f32 spread = 0;
    f32 speed_variation = 0;
    f32 rate = 12;
    f32 life = 1;
    f32 life_variation = 0;
    f32 size = 0.1f;
    f32 color[4] = {1, 1, 1, 1};
    u32 max_particles = 64;
    bool additive = false;
    u32 texture = 0xffffffffu;  // bindless index for the billboard texture
    u32 subtex_frames = 1;      // flipbook frames (1 = whole texture)
    u32 subtex_cols = 1;
    u32 subtex_rows = 1;
    bool has_ramp = false;      // BSPSysSimpleColorModifier colour-over-life
    f32 ramp_key[6] = {0, 1, 0.33f, 0.33f, 0.66f, 0.66f};
    f32 ramp_color[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    f32 spawn_accumulator = 0;
    u32 rng = 1;
    u64 last_frame = 0;
    base::Vector<Particle> particles;
  };

  static constexpr u32 kMaxTotal = 4096;        // billboards across all pools
  static constexpr u32 kMaxPerPool = 128;
  static constexpr u64 kEvictAfterFrames = 300;  // untouched pools get dropped

  void Step(Pool& pool);

  f32 dt_ = 0;
  Vec3 camera_{};
  f32 max_distance_ = 60.0f;
  u64 frame_ = 0;
  u32 live_ = 0;
  base::UnorderedMap<u64, Pool> pools_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_PARTICLE_EMITTERS_H_

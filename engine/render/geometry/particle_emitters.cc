#include "render/geometry/particle_emitters.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace rec::render {
namespace {

// xorshift32 to [0, 1).
f32 RandUnit(u32& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return static_cast<f32>(state >> 8) * (1.0f / 16777216.0f);
}

f32 RandSigned(u32& state) { return RandUnit(state) * 2.0f - 1.0f; }

u64 HashCombine(u64 h, u64 v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

f32 LengthSq(const Vec3& v) { return v.x * v.x + v.y * v.y + v.z * v.z; }

f32 SafeLerpT(f32 t, f32 a, f32 b) { return b - a > 1e-5f ? std::clamp((t - a) / (b - a), 0.0f, 1.0f) : 1.0f; }

// Random direction in a cone of half angle `spread` around `axis`.
Vec3 ConeDirection(const Vec3& axis, f32 spread, u32& rng) {
  if (spread <= 1e-3f) return axis;
  Vec3 ref = std::abs(axis.y) < 0.95f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
  Vec3 t = Normalize(Cross(ref, axis));
  Vec3 b = Cross(axis, t);
  f32 angle = spread * RandUnit(rng);
  f32 azimuth = 6.2831853f * RandUnit(rng);
  f32 s = std::sin(angle);
  return Normalize(axis * std::cos(angle) + t * (s * std::cos(azimuth)) +
                   b * (s * std::sin(azimuth)));
}

}  // namespace

void ParticleEmitterSim::BeginFrame(f32 dt, const Vec3& camera, f32 max_distance) {
  dt_ = std::min(dt, 0.05f);  // clamp hitches
  camera_ = camera;
  max_distance_ = max_distance;
  ++frame_;
}

void ParticleEmitterSim::AddInstance(u64 mesh_key,
                                     const base::Vector<asset::ParticleEmitter>& emitters,
                                     const Mat4& transform) {
  for (size_t e = 0; e < emitters.size(); ++e) {
    const asset::ParticleEmitter& emitter = emitters[e];
    Vec3 origin = TransformPoint(
        transform, {emitter.position[0], emitter.position[1], emitter.position[2]});
    if (LengthSq(origin - camera_) > max_distance_ * max_distance_) continue;

    // Stable key: mesh + emitter index + world position on a 1 cm grid.
    u64 key = HashCombine(mesh_key, e);
    key = HashCombine(key, static_cast<u64>(static_cast<i64>(origin.x * 100.0f)));
    key = HashCombine(key, static_cast<u64>(static_cast<i64>(origin.y * 100.0f)));
    key = HashCombine(key, static_cast<u64>(static_cast<i64>(origin.z * 100.0f)));

    Pool* pool = pools_.find(key);
    if (!pool) {
      Pool fresh;
      fresh.origin = origin;
      fresh.velocity = TransformDir(
          transform, {emitter.velocity[0], emitter.velocity[1], emitter.velocity[2]});
      fresh.gravity =
          TransformDir(transform, {emitter.gravity[0], emitter.gravity[1], emitter.gravity[2]});
      // World AABB of the oriented emit volume.
      Vec3 ex = TransformDir(transform, {emitter.extent[0], 0, 0});
      Vec3 ey = TransformDir(transform, {0, emitter.extent[1], 0});
      Vec3 ez = TransformDir(transform, {0, 0, emitter.extent[2]});
      fresh.extent = {std::abs(ex.x) + std::abs(ey.x) + std::abs(ez.x),
                      std::abs(ex.y) + std::abs(ey.y) + std::abs(ez.y),
                      std::abs(ex.z) + std::abs(ey.z) + std::abs(ez.z)};
      // Uniform scale of the instance transform, for the scalar quantities.
      f32 scale = std::sqrt(LengthSq(TransformDir(transform, {1, 0, 0})));
      fresh.spread = emitter.spread;
      fresh.speed_variation = emitter.speed_variation * scale;
      fresh.rate = emitter.rate;
      fresh.life = emitter.life;
      fresh.life_variation = emitter.life_variation;
      fresh.size = emitter.size * scale;
      for (int k = 0; k < 4; ++k) fresh.color[k] = emitter.color[k];
      fresh.max_particles = std::min(emitter.max_particles, kMaxPerPool);
      fresh.additive = emitter.additive;
      // The renderer rewrote emitter.texture to a bindless index at upload.
      fresh.texture = static_cast<u32>(emitter.texture);
      fresh.subtex_frames = std::max(emitter.subtex_frames, 1u);
      fresh.subtex_cols = std::clamp(emitter.subtex_cols, 1u, 15u);
      fresh.subtex_rows = std::clamp(emitter.subtex_rows, 1u, 15u);
      fresh.has_ramp = emitter.has_color_ramp;
      for (int j = 0; j < 6; ++j) fresh.ramp_key[j] = emitter.ramp_key[j];
      for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < 4; ++k) fresh.ramp_color[j][k] = emitter.ramp_color[j][k];
      }
      fresh.rng = static_cast<u32>(key ^ (key >> 32)) | 1u;
      REC_DEBUG("particles: emitter pool at ({:.1f}, {:.1f}, {:.1f}) rate={:.1f} additive={}",
                origin.x, origin.y, origin.z, fresh.rate, fresh.additive);
      pools_.emplace(key, std::move(fresh));
      pool = pools_.find(key);
    }
    pool->last_frame = frame_;
  }
}

void ParticleEmitterSim::Step(Pool& pool) {
  for (size_t i = 0; i < pool.particles.size();) {
    Particle& particle = pool.particles[i];
    particle.age += dt_;
    if (particle.age >= particle.life) {
      particle = pool.particles.back();
      pool.particles.pop_back();
      continue;
    }
    particle.prev = particle.pos;
    particle.vel += pool.gravity * dt_;
    particle.pos += particle.vel * dt_;
    ++i;
  }

  pool.spawn_accumulator = std::min(pool.spawn_accumulator + pool.rate * dt_, 8.0f);
  f32 speed = std::sqrt(LengthSq(pool.velocity));
  Vec3 axis = speed > 1e-4f ? pool.velocity * (1.0f / speed) : Vec3{0, 1, 0};
  while (pool.spawn_accumulator >= 1.0f && pool.particles.size() < pool.max_particles) {
    pool.spawn_accumulator -= 1.0f;
    Particle particle;
    particle.pos = pool.origin + Vec3{pool.extent.x * RandSigned(pool.rng),
                                      pool.extent.y * RandSigned(pool.rng),
                                      pool.extent.z * RandSigned(pool.rng)};
    particle.prev = particle.pos;
    f32 birth_speed = std::max(speed + pool.speed_variation * RandSigned(pool.rng), 0.0f);
    particle.vel = ConeDirection(axis, pool.spread, pool.rng) * birth_speed;
    particle.life =
        std::max(pool.life + pool.life_variation * RandSigned(pool.rng), 0.05f);
    particle.size = pool.size * (0.75f + 0.5f * RandUnit(pool.rng));
    pool.particles.push_back(particle);
  }
}

void ParticleEmitterSim::Simulate(base::Vector<ParticleInstance>* lit,
                                  base::Vector<ParticleInstance>* additive) {
  live_ = 0;
  base::Vector<u64> stale;
  for (auto entry : pools_) {
    Pool& pool = entry.value;
    if (pool.last_frame != frame_) {
      if (frame_ - pool.last_frame > kEvictAfterFrames) stale.push_back(entry.key);
      continue;
    }
    Step(pool);
    for (const Particle& particle : pool.particles) {
      if (live_ >= kMaxTotal) break;
      f32 t = particle.age / particle.life;
      // Quick fade in, longer fade out; flames shrink with age, smoke grows.
      f32 fade = std::min(t * 6.0f, 1.0f) * std::min((1.0f - t) * 2.5f, 1.0f);
      // BSPSysSimpleColorModifier colour-over-life: a 3-stop rgba gradient plus
      // an alpha fade in/out, overriding the fixed class tint when authored.
      f32 ramp[4] = {pool.color[0], pool.color[1], pool.color[2], pool.color[3]};
      if (pool.has_ramp) {
        const f32* k = pool.ramp_key;
        const auto seg = [&](int a, int b, f32 s) {
          for (int c = 0; c < 4; ++c)
            ramp[c] = pool.ramp_color[a][c] + (pool.ramp_color[b][c] - pool.ramp_color[a][c]) * s;
        };
        if (t <= k[2]) seg(0, 0, 0.0f);
        else if (t < k[3]) seg(0, 1, SafeLerpT(t, k[2], k[3]));
        else if (t <= k[4]) seg(1, 1, 0.0f);
        else if (t < k[5]) seg(1, 2, SafeLerpT(t, k[4], k[5]));
        else seg(2, 2, 0.0f);
        f32 fade_in = k[0] > 1e-4f ? std::clamp(t / k[0], 0.0f, 1.0f) : 1.0f;
        f32 fade_out = k[1] < 0.9999f ? std::clamp((1.0f - t) / (1.0f - k[1]), 0.0f, 1.0f) : 1.0f;
        ramp[3] *= fade_in * fade_out;
      }
      ParticleInstance inst;
      inst.pos[0] = particle.pos.x;
      inst.pos[1] = particle.pos.y;
      inst.pos[2] = particle.pos.z;
      inst.prev_pos[0] = particle.prev.x;
      inst.prev_pos[1] = particle.prev.y;
      inst.prev_pos[2] = particle.prev.z;
      // Packed billboard texturing: bindless index in the low 16 bits, then the
      // flipbook frame (by life fraction) and the atlas grid dims. 0xffffffff
      // stays the untextured sentinel (the PS checks the low 16 bits).
      if (pool.texture == 0xffffffffu) {
        inst.tex = 0xffffffffu;
      } else {
        u32 frame_index = 0;
        if (pool.subtex_frames > 1) {
          frame_index = std::min(static_cast<u32>(t * static_cast<f32>(pool.subtex_frames)),
                                 pool.subtex_frames - 1);
        }
        inst.tex = (pool.texture & 0xffffu) | (frame_index & 0xffu) << 16 |
                   (pool.subtex_cols & 0xfu) << 24 | (pool.subtex_rows & 0xfu) << 28;
      }
      if (pool.additive) {
        inst.size = particle.size * (1.0f - 0.35f * t);
        if (pool.has_ramp) {
          // Premultiply the authored alpha into an HDR radiance so it blooms.
          for (int k = 0; k < 3; ++k) inst.color[k] = ramp[k] * ramp[3] * 3.0f;
        } else {
          for (int k = 0; k < 3; ++k) inst.color[k] = pool.color[k] * fade;
        }
        inst.color[3] = 1.0f;
        additive->push_back(inst);
      } else {
        inst.size = particle.size * (1.0f + 0.8f * t);
        if (pool.has_ramp) {
          for (int k = 0; k < 3; ++k) inst.color[k] = ramp[k];
          inst.color[3] = ramp[3];
        } else {
          for (int k = 0; k < 3; ++k) inst.color[k] = pool.color[k];
          inst.color[3] = pool.color[3] * fade;
        }
        lit->push_back(inst);
      }
      ++live_;
    }
  }
  for (u64 key : stale) pools_.erase(key);
}

}  // namespace rec::render

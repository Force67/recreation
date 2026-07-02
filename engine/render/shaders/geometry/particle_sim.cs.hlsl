#include "rhi_bindings.hlsli"
// GPU particle simulation: integrates a persistent particle state buffer and
// writes the camera-facing billboard instances the particle.vs draws. Every
// particle respawns at the emitter when its life runs out, so the live set is a
// fixed N and the cpu never touches per-particle data. This is the gpu-driven
// particle path; scaling to hundreds of thousands of embers is what the cpu
// fountain could not do.
struct ParticleState {
  float3 pos;
  float life;
  float3 vel;
  float max_life;
  float3 color;
  float size;
  uint seed;
  float3 pad;
};

struct ParticleInstance {  // matches render::ParticleInstance / particle.vs
  float3 pos;
  float size;
  float4 color;
  float3 prev_pos;
  float pad;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<ParticleState> state : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<ParticleInstance> instances : register(u1, space0);

struct PushData {
  float3 emitter;
  float dt;
  float gravity;
  float spawn_speed;
  float life_min;
  float life_range;
  float size_min;
  float size_range;
  uint count;
  uint frame;
};
PUSH_CONSTANTS(PushData, push);

uint Pcg(inout uint s) {
  s = s * 747796405u + 2891336453u;
  uint w = ((s >> ((s >> 28) + 4u)) ^ s) * 277803737u;
  return (w >> 22) ^ w;
}
float Rng(inout uint s) { return float(Pcg(s) & 0xffffffu) / 16777216.0; }

void Respawn(inout ParticleState p, inout uint seed) {
  p.pos = push.emitter;
  float ang = Rng(seed) * 6.2831853;
  float spread = Rng(seed) * 1.4;
  p.vel = float3(cos(ang) * spread, push.spawn_speed + Rng(seed) * 2.0, sin(ang) * spread);
  p.max_life = push.life_min + Rng(seed) * push.life_range;
  p.life = p.max_life;
  p.size = push.size_min + Rng(seed) * push.size_range;
  p.color = float3(1.0, 0.45 + Rng(seed) * 0.3, 0.1);  // warm embers
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  if (i >= push.count) return;
  ParticleState p = state[i];

  uint seed = p.seed;
  if (seed == 0u) {  // first touch: seed and stagger the initial lives
    seed = (i * 747796405u) + 2891336453u + push.frame;
    Respawn(p, seed);
    p.life = Rng(seed) * p.max_life;  // spread across the lifecycle so it streams
  }

  float3 old_pos = p.pos;
  p.life -= push.dt;
  if (p.life <= 0.0) {
    Respawn(p, seed);
    old_pos = p.pos;  // teleported to the emitter: no motion across the respawn
  } else {
    p.vel.y -= push.gravity * push.dt;
    p.pos += p.vel * push.dt;
  }
  p.seed = seed;
  state[i] = p;

  float t = p.life / max(p.max_life, 1e-3);  // 1 at birth, 0 at death
  ParticleInstance inst;
  inst.pos = p.pos;
  inst.size = p.size * (1.3 - 0.3 * t);
  inst.color = float4(p.color, t * t * 0.8);
  inst.prev_pos = old_pos;
  inst.pad = 0.0;
  instances[i] = inst;
}

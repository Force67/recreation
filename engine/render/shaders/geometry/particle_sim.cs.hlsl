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
  uint tex;  // bindless texture index; the gpu fountain stays procedural
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
  uint mode;        // 0 ember fountain, 1 fire (buoyant flames + embers)
  float radius;     // emitter disk radius (fire)
  float intensity;  // emissive scale (fire)
  float time;
  float pad0;
  float pad1;
};
PUSH_CONSTANTS(PushData, push);

// Cheap divergence-free-ish turbulence: curl of three phase-shifted value
// noises. Good enough for flame licking; a real curl-noise texture can slot in
// later without touching the integration.
float Hash(float3 p) {
  p = frac(p * 0.3183099 + 0.1);
  p *= 17.0;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float Noise(float3 x) {
  float3 i = floor(x);
  float3 f = frac(x);
  f = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(lerp(Hash(i + float3(0, 0, 0)), Hash(i + float3(1, 0, 0)), f.x),
                   lerp(Hash(i + float3(0, 1, 0)), Hash(i + float3(1, 1, 0)), f.x), f.y),
              lerp(lerp(Hash(i + float3(0, 0, 1)), Hash(i + float3(1, 0, 1)), f.x),
                   lerp(Hash(i + float3(0, 1, 1)), Hash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float3 Turbulence(float3 p, float t) {
  float3 q = p * 2.3 + float3(0.0, -t * 1.1, 0.0);  // field scrolls downward = flames lick up
  float e = 0.35;
  // Pseudo-curl: gradients of two offset noises crossed.
  float3 g1 = float3(Noise(q + float3(e, 0, 0)) - Noise(q - float3(e, 0, 0)),
                     Noise(q + float3(0, e, 0)) - Noise(q - float3(0, e, 0)),
                     Noise(q + float3(0, 0, e)) - Noise(q - float3(0, 0, e)));
  float3 g2 = float3(Noise(q + float3(31.4 + e, 17.7, 9.2)) - Noise(q + float3(31.4 - e, 17.7, 9.2)),
                     Noise(q + float3(31.4, 17.7 + e, 9.2)) - Noise(q + float3(31.4, 17.7 - e, 9.2)),
                     Noise(q + float3(31.4, 17.7, 9.2 + e)) - Noise(q + float3(31.4, 17.7, 9.2 - e)));
  return cross(g1, g2) * 40.0;
}

// Blackbody-ish flame ramp by normalized age (1 birth .. 0 death).
float3 FlameColor(float t) {
  float3 hot = float3(1.0, 0.86, 0.55);   // near-white core
  float3 mid = float3(1.0, 0.45, 0.08);   // orange
  float3 cool = float3(0.55, 0.08, 0.01); // dying red
  return t > 0.6 ? lerp(mid, hot, (t - 0.6) / 0.4) : lerp(cool, mid, t / 0.6);
}

uint Pcg(inout uint s) {
  s = s * 747796405u + 2891336453u;
  uint w = ((s >> ((s >> 28) + 4u)) ^ s) * 277803737u;
  return (w >> 22) ^ w;
}
float Rng(inout uint s) { return float(Pcg(s) & 0xffffffu) / 16777216.0; }

// A stable per-particle fraction becomes embers in fire mode: small, bright,
// long-lived sparks that ride the updraft, vs the short soft flame puffs.
bool IsEmber(uint index) { return (index % 8u) == 0u; }

void Respawn(inout ParticleState p, uint index, inout uint seed) {
  if (push.mode == 1u) {  // fire
    float ang = Rng(seed) * 6.2831853;
    float rad = sqrt(Rng(seed)) * push.radius;
    p.pos = push.emitter + float3(cos(ang) * rad, Rng(seed) * 0.05, sin(ang) * rad);
    if (IsEmber(index)) {
      p.vel = float3(cos(ang) * 0.4 * Rng(seed), 1.5 + Rng(seed) * 2.5, sin(ang) * 0.4 * Rng(seed));
      p.max_life = 1.2 + Rng(seed) * 1.6;
      p.size = 0.012 + Rng(seed) * 0.018;
    } else {
      p.vel = float3(cos(ang) * 0.15, 0.3 + Rng(seed) * 0.6, sin(ang) * 0.15);
      p.max_life = 0.45 + Rng(seed) * 0.55;
      p.size = (push.size_min + Rng(seed) * push.size_range);
    }
    p.life = p.max_life;
    p.color = float3(1.0, 0.5, 0.12);
    return;
  }
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
    Respawn(p, i, seed);
    p.life = Rng(seed) * p.max_life;  // spread across the lifecycle so it streams
  }

  float3 old_pos = p.pos;
  p.life -= push.dt;
  if (p.life <= 0.0) {
    Respawn(p, i, seed);
    old_pos = p.pos;  // teleported to the emitter: no motion across the respawn
  } else if (push.mode == 1u) {
    bool ember = IsEmber(i);
    // Hot gas rises, drag brakes it, turbulence makes it lick and curl.
    float t = p.life / max(p.max_life, 1e-3);
    float buoyancy = ember ? 1.2 : 2.4 * t;  // cooling flame loses lift
    p.vel.y += buoyancy * push.dt;
    p.vel += Turbulence(p.pos, push.time) * (ember ? 0.5 : 1.0) * push.dt;
    p.vel *= exp(-(ember ? 0.6 : 1.8) * push.dt);  // drag
    p.pos += p.vel * push.dt;
  } else {
    p.vel.y -= push.gravity * push.dt;
    p.pos += p.vel * push.dt;
  }
  p.seed = seed;
  state[i] = p;

  float t = p.life / max(p.max_life, 1e-3);  // 1 at birth, 0 at death
  ParticleInstance inst;
  inst.pos = p.pos;
  if (push.mode == 1u) {
    if (IsEmber(i)) {
      inst.size = p.size;
      // Embers flicker as they tumble and cool from yellow to deep red.
      float flick = 0.65 + 0.35 * Noise(p.pos * 40.0 + push.time * 12.0);
      inst.color = float4(FlameColor(0.35 + 0.4 * t) * push.intensity * 6.0 * flick, t);
    } else {
      inst.size = p.size * (1.7 - 0.7 * t);  // the puff expands as it rises
      float fade = t * t;
      inst.color = float4(FlameColor(t) * push.intensity * 4.0 * fade, fade * 0.85);
    }
  } else {
    inst.size = p.size * (1.3 - 0.3 * t);
    inst.color = float4(p.color, t * t * 0.8);
  }
  inst.prev_pos = old_pos;
  inst.tex = 0xffffffffu;  // procedural sprite
  instances[i] = inst;
}

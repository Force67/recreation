#include "rhi_bindings.hlsli"
// Depth of field, stage 1: circle of confusion + autofocus. The focus
// distance lives in a tiny storage buffer and eases toward the center-pixel
// depth every frame (no cpu readback); coc is signed (negative = near field)
// in output pixels, clamped to max_coc.
struct CocPush {
  uint2 size;
  float near_plane;
  float aperture;       // coc pixels per unit of defocus, the "f-stop" feel
  float max_coc;        // pixels
  float focus_speed;    // per-frame ease toward the autofocus target
  float focus_override; // > 0: fixed focus distance, no autofocus
  float pad0;
};
PUSH_CONSTANTS(CocPush, pc);

[[vk::image_format("r16f")]] [[vk::binding(0, 0)]] RWTexture2D<float> coc_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float> focus_state : register(u2, space0);  // [0] distance

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;

  // Thread (0,0) advances the autofocus before anyone reads it; the race with
  // other groups is a one-frame lag at worst, invisible through the ease.
  if (all(id.xy == 0)) {
    float target = pc.focus_override;
    if (target <= 0.0) {
      float cd = depth_map.Load(int3(int2(pc.size) / 2, 0));
      target = cd > 0.0 ? pc.near_plane / cd : 100.0;
    }
    float current = focus_state[0];
    if (current <= 0.0) current = target;
    focus_state[0] = lerp(current, target, pc.focus_speed);
  }

  float focus = max(focus_state[0], pc.near_plane);
  float depth = depth_map.Load(int3(id.xy, 0));
  float z = depth > 0.0 ? pc.near_plane / depth : 1e6;
  // Thin lens: coc grows with relative defocus; signed so near blur bleeds.
  float coc = pc.aperture * (z - focus) / max(z, 1e-3);
  coc_out[id.xy] = clamp(coc, -pc.max_coc, pc.max_coc);
}

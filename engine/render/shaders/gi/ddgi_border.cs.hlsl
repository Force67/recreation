#include "rhi_bindings.hlsli"
// Octahedral border duplication so bilinear taps at probe edges wrap
// correctly. One thread per atlas texel, border texels copy their wrapped
// interior source.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2DArray<float4> atlas : register(u0, space0);

struct PushData {
  uint texels;  // interior resolution
  uint probes_x;
  uint probes_y;
  uint pad;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint stride = push.texels + 2;
  if (id.x >= push.probes_x * stride || id.y >= push.probes_y * stride) return;
  uint2 in_probe = uint2(id.x % stride, id.y % stride);
  bool border_x = in_probe.x == 0 || in_probe.x == stride - 1;
  bool border_y = in_probe.y == 0 || in_probe.y == stride - 1;
  if (!border_x && !border_y) return;

  uint2 base = uint2(id.x - in_probe.x, id.y - in_probe.y);
  uint n = push.texels;
  uint2 src;
  if (border_x && border_y) {
    // Corners wrap diagonally across the probe.
    src = uint2(in_probe.x == 0 ? n : 1, in_probe.y == 0 ? n : 1);
  } else if (border_x) {
    // Column border: mirror the row, wrap the column.
    src = uint2(in_probe.x == 0 ? 1 : n, n + 1 - in_probe.y);
  } else {
    src = uint2(n + 1 - in_probe.x, in_probe.y == 0 ? 1 : n);
  }
  atlas[id] = atlas[uint3(base + src, id.z)];
}

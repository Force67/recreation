#include "rhi_bindings.hlsli"
// Min-depth reduction for occlusion culling. With block == 1 it copies depth
// 1:1 (snapshot last frame's depth); with block == 8 it reduces to a coarse
// hi-z where each texel is the FARTHEST surface (the minimum in reversed-z) over
// its block, so a bounds test against it is conservative: a sphere is occluded
// only if it is behind even the farthest occluder in its screen footprint.
[[vk::image_format("r32f")]] [[vk::binding(0, 0)]] RWTexture2D<float> dst : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> src : register(t1, space0);

struct PushData {
  uint2 dst_size;
  uint block;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= push.dst_size.x || id.y >= push.dst_size.y) return;
  int2 base = int2(id.xy) * int(push.block);
  float m = src.Load(int3(base, 0));
  for (uint y = 0; y < push.block; ++y) {
    for (uint x = 0; x < push.block; ++x) {
      m = min(m, src.Load(int3(base + int2(x, y), 0)));
    }
  }
  dst[id.xy] = m;
}

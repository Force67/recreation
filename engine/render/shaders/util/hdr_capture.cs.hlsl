#include "rhi_bindings.hlsli"
// Copies the resolved linear-hdr scene (pre-tonemap fp16) into a host-visible
// float buffer so the cpu can write it out as a radiance .hdr file. One thread
// per pixel; the buffer is row-major rgba32f.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> out_buf : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> src : register(t1, space0);

struct PushData {
  uint width;
  uint height;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= push.width || id.y >= push.height) return;
  out_buf[id.y * push.width + id.x] = src.Load(int3(id.xy, 0));
}

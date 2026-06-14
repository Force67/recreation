// Copies the resolved linear-hdr scene (pre-tonemap fp16) into a host-visible
// float buffer so the cpu can write it out as a radiance .hdr file. One thread
// per pixel; the buffer is row-major rgba32f.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> out_buf;
[[vk::binding(1, 0)]] Texture2D<float4> src;

struct PushData {
  uint width;
  uint height;
};
[[vk::push_constant]] PushData push;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= push.width || id.y >= push.height) return;
  out_buf[id.y * push.width + id.x] = src.Load(int3(id.xy, 0));
}

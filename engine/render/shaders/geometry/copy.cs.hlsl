// Snapshots the opaque scene color so the transparent pass can refract
// through it while rendering on top. Depth comes from the prepass export.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> dst_color : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D src_color : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState src_color_sampler : register(s1, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  dst_color.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  dst_color[id.xy] = src_color.Load(int3(id.xy, 0));
}

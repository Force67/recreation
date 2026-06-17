// Snapshots the opaque scene color and depth so the transparent pass can
// refract through them while rendering on top.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> dst_color;
[[vk::image_format("r32f")]] [[vk::binding(1, 0)]] RWTexture2D<float> dst_depth;
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D src_color;
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState src_color_sampler;
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D src_depth;
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState src_depth_sampler;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  dst_color.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  dst_color[id.xy] = src_color.Load(int3(id.xy, 0));
  dst_depth[id.xy] = src_depth.Load(int3(id.xy, 0)).r;
}

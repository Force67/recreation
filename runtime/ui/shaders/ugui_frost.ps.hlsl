// ultragui frosted-glass fragment stage. Fills a rounded rect with a sample of
// the pre-blurred backdrop (the scene behind the UI, Gaussian-blurred by the
// renderer into a small texture) modulated by the widget's vertex color. The
// widget's own translucent background composites on top for the tint. SDF
// rounding matches the quad pipeline so frosted panels keep their corners.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D backdrop;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState backdrop_sampler;

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float2 screen_uv : TEXCOORD1;
  [[vk::location(2)]] float4 color : COLOR0;
  [[vk::location(3)]] float4 corner_radii : TEXCOORD2;
  [[vk::location(4)]] float softness : TEXCOORD3;
  [[vk::location(5)]] float2 half_size : TEXCOORD4;
};

float SdfRoundedRect4(float2 p, float2 b, float4 radii) {
  float radius =
      (p.x > 0.0) ? ((p.y > 0.0) ? radii.z : radii.y) : ((p.y > 0.0) ? radii.w : radii.x);
  float2 q = abs(p) - b + radius;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float4 main(PsIn input) : SV_Target0 {
  // The backdrop texture is already Gaussian-blurred; a single bilinear tap is
  // enough for a smooth frosted read.
  float3 blurred = backdrop.Sample(backdrop_sampler, input.screen_uv).rgb;
  float4 color = float4(blurred * input.color.rgb, input.color.a);

  if (input.half_size.x > 0.0 && input.half_size.y > 0.0) {
    float2 local = (input.uv * 2.0 - 1.0) * input.half_size;
    float d = SdfRoundedRect4(local, input.half_size, input.corner_radii);
    float aa = max(fwidth(d) * 0.75, abs(input.softness));
    float alpha = 1.0 - smoothstep(-aa, aa, d);
    color.a *= alpha;
  }
  return color;
}

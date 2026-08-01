// ultragui text pipeline fragment stage. HLSL port of shaders/text.frag: the
// glyph atlas is single-channel R8 alpha, modulated by the vertex color.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState tex_sampler;

float4 main(float4 sv_position : SV_Position, [[vk::location(0)]] float2 uv : TEXCOORD0,
            [[vk::location(1)]] float4 color : COLOR0) : SV_Target0 {
  float alpha = tex.Sample(tex_sampler, uv).r;
  return float4(color.rgb, color.a * alpha);
}

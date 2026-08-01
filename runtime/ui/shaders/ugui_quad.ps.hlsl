// ultragui quad pipeline fragment stage. HLSL port of shaders/quad.frag: SDF
// rounded rectangle with optional gradient and border. Colors are written as
// authored (the swapchain is UNORM and the engine owns the transfer function,
// so the UI composites in the same encoded space as the debug overlay).

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState tex_sampler;

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 color : COLOR0;
  [[vk::location(2)]] float4 color2 : COLOR1;
  [[vk::location(3)]] float4 corner_radii : TEXCOORD1;
  [[vk::location(4)]] float softness : TEXCOORD2;
  [[vk::location(5)]] float2 half_size : TEXCOORD3;
  [[vk::location(6)]] float border_width : TEXCOORD4;
  [[vk::location(7)]] float4 border_color : COLOR2;
};

float SdfRoundedRect4(float2 p, float2 b, float4 radii) {
  float radius =
      (p.x > 0.0) ? ((p.y > 0.0) ? radii.z : radii.y) : ((p.y > 0.0) ? radii.w : radii.x);
  float2 q = abs(p) - b + radius;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float4 main(PsIn input) : SV_Target0 {
  float4 tex_color = tex.Sample(tex_sampler, input.uv);

  // Linear gradient: interpolate between color and color2 along the V axis.
  float4 base_color = lerp(input.color, input.color2, input.uv.y);
  float4 color = base_color * tex_color;

  if (input.half_size.x > 0.0 && input.half_size.y > 0.0) {
    float2 local = (input.uv * 2.0 - 1.0) * input.half_size;
    float d = SdfRoundedRect4(local, input.half_size, input.corner_radii);

    float soft = abs(input.softness);
    float aa = max(fwidth(d) * 0.75, soft);
    float alpha;
    if (input.softness < 0.0) {
      alpha = smoothstep(-aa, 0.0, d);
    } else {
      alpha = 1.0 - smoothstep(-aa, aa, d);
    }
    color.a *= alpha;

    if (input.border_width > 0.0 && input.border_color.a > 0.0) {
      float2 inner_half = input.half_size - float2(input.border_width, input.border_width);
      float bw = input.border_width;
      float4 inner_radii = max(input.corner_radii - float4(bw, bw, bw, bw), float4(0, 0, 0, 0));
      float d_inner = SdfRoundedRect4(local, inner_half, inner_radii);
      float inner_aa = fwidth(d_inner) * 0.75;
      float inner_alpha = 1.0 - smoothstep(-inner_aa, inner_aa, d_inner);

      float border_mask = alpha * (1.0 - inner_alpha);
      float4 border_col = input.border_color;
      border_col.a *= border_mask;

      float4 fill = color;
      fill.a *= inner_alpha;

      color = fill + border_col * (1.0 - fill.a);
      color.a = fill.a + border_col.a * (1.0 - fill.a);
    }
  }

  return color;
}

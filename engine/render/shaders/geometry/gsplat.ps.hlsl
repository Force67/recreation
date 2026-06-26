// Evaluates the gaussian through its 2d conic and alpha blends. The vertex
// shader handed over the pixel offset from the splat center and the inverse 2d
// covariance (conic a,b,c).
struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 offset : TEXCOORD0;
  [[vk::location(1)]] float3 conic : TEXCOORD1;
  [[vk::location(2)]] float4 color : COLOR0;  // rgb, opacity
};

float4 main(PsIn input) : SV_Target0 {
  float power = -0.5 * (input.conic.x * input.offset.x * input.offset.x +
                        input.conic.z * input.offset.y * input.offset.y) -
                input.conic.y * input.offset.x * input.offset.y;
  if (power > 0.0) discard;
  float alpha = min(0.99, input.color.a * exp(power));
  if (alpha < 1.0 / 255.0) discard;
  return float4(input.color.rgb, alpha);
}

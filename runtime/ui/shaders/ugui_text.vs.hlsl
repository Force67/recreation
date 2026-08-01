// ultragui text pipeline vertex stage. HLSL port of shaders/text.vert. Shares
// the Vertex2D layout with the quad pipeline but only reads position, uv and
// color; the unused attributes are still declared by the shared vertex input.

struct PushData {
  float2 scale;
  float2 translate;
};
[[vk::push_constant]] PushData push;

struct VsIn {
  [[vk::location(0)]] float2 pos : POSITION;
  [[vk::location(1)]] float2 uv : TEXCOORD0;
  [[vk::location(2)]] uint color : COLOR0;
};

struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 color : COLOR0;
};

VsOut main(VsIn input) {
  VsOut output;
  output.sv_position = float4(input.pos * push.scale + push.translate, 0.0, 1.0);
  output.uv = input.uv;
  output.color = float4(float(input.color & 0xFFu) / 255.0, float((input.color >> 8) & 0xFFu) / 255.0,
                        float((input.color >> 16) & 0xFFu) / 255.0,
                        float((input.color >> 24) & 0xFFu) / 255.0);
  return output;
}

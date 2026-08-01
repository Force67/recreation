// ultragui quad pipeline (SDF rounded rects, gradients, borders). HLSL port of
// libultragui's shaders/quad.vert; matches the Vertex2D layout 1:1. The push
// constant maps screen pixels to clip space: clip = pos * scale + translate
// with scale = (2/w, 2/h), translate = (-1, -1).

struct PushData {
  float2 scale;
  float2 translate;
};
[[vk::push_constant]] PushData push;

struct VsIn {
  [[vk::location(0)]] float2 pos : POSITION;
  [[vk::location(1)]] float2 uv : TEXCOORD0;
  [[vk::location(2)]] uint color : COLOR0;
  [[vk::location(3)]] uint color2 : COLOR1;
  [[vk::location(4)]] uint corner_radii : TEXCOORD1;
  [[vk::location(5)]] float softness : TEXCOORD2;
  [[vk::location(6)]] float2 half_size : TEXCOORD3;
  [[vk::location(7)]] float border_width : TEXCOORD4;
  [[vk::location(8)]] uint border_color : COLOR2;
};

struct VsOut {
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

float4 UnpackColor(uint c) {
  return float4(float(c & 0xFFu) / 255.0, float((c >> 8) & 0xFFu) / 255.0,
                float((c >> 16) & 0xFFu) / 255.0, float((c >> 24) & 0xFFu) / 255.0);
}

VsOut main(VsIn input) {
  VsOut output;
  output.sv_position = float4(input.pos * push.scale + push.translate, 0.0, 1.0);
  output.uv = input.uv;
  output.color = UnpackColor(input.color);
  output.color2 = UnpackColor(input.color2);
  output.corner_radii = float4(float(input.corner_radii & 0xFFu),
                               float((input.corner_radii >> 8) & 0xFFu),
                               float((input.corner_radii >> 16) & 0xFFu),
                               float((input.corner_radii >> 24) & 0xFFu));
  output.softness = input.softness;
  output.half_size = input.half_size;
  output.border_width = input.border_width;
  output.border_color = UnpackColor(input.border_color);
  return output;
}

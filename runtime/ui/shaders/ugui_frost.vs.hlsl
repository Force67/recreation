// ultragui frosted-glass (backdrop blur) vertex stage. Same Vertex2D layout and
// screen->clip mapping as the quad pipeline, but it also emits a screen-space UV
// so the fragment stage can sample the pre-blurred backdrop captured behind the
// UI. clip = pos * scale + translate; screen_uv = pos * scale * 0.5 (0..1).

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
  [[vk::location(1)]] float2 screen_uv : TEXCOORD1;
  [[vk::location(2)]] float4 color : COLOR0;
  [[vk::location(3)]] float4 corner_radii : TEXCOORD2;
  [[vk::location(4)]] float softness : TEXCOORD3;
  [[vk::location(5)]] float2 half_size : TEXCOORD4;
};

float4 UnpackColor(uint c) {
  return float4(float(c & 0xFFu) / 255.0, float((c >> 8) & 0xFFu) / 255.0,
                float((c >> 16) & 0xFFu) / 255.0, float((c >> 24) & 0xFFu) / 255.0);
}

VsOut main(VsIn input) {
  VsOut output;
  output.sv_position = float4(input.pos * push.scale + push.translate, 0.0, 1.0);
  output.uv = input.uv;
  output.screen_uv = input.pos * push.scale * 0.5;  // 0..1 across the framebuffer
  output.color = UnpackColor(input.color);
  output.corner_radii = float4(float(input.corner_radii & 0xFFu),
                               float((input.corner_radii >> 8) & 0xFFu),
                               float((input.corner_radii >> 16) & 0xFFu),
                               float((input.corner_radii >> 24) & 0xFFu));
  output.softness = input.softness;
  output.half_size = input.half_size;
  return output;
}

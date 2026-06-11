#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene;

// Narkowicz ACES fit. Cheap, no LUT, good enough until a proper grading
// stage with exposure and white balance lands.
vec3 TonemapAces(vec3 x) {
  return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// The swapchain is UNORM, the engine owns the transfer function.
vec3 SrgbEncode(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
  vec3 hdr = texture(scene, in_uv).rgb;
  out_color = vec4(SrgbEncode(TonemapAces(hdr)), 1.0);
}

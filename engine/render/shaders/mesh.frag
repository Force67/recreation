#version 450

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec4 in_curr_clip;
layout(location = 2) in vec4 in_prev_clip;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_motion;

void main() {
  vec3 n = normalize(in_normal);
  vec3 l = normalize(vec3(0.4, 0.8, 0.3));
  float ndl = max(dot(n, l), 0.0);
  vec3 base = vec3(0.55, 0.6, 0.7);
  out_color = vec4(base * (0.15 + 0.85 * ndl), 1.0);

  // Uv offset from this pixel to where the surface was last frame.
  vec2 curr = in_curr_clip.xy / in_curr_clip.w;
  vec2 prev = in_prev_clip.xy / in_prev_clip.w;
  out_motion = (prev - curr) * 0.5;
}

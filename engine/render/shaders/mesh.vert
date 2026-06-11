#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(set = 0, binding = 0) uniform FrameGlobals {
  mat4 view_proj;
  mat4 prev_view_proj;
  vec2 jitter;       // ndc units, applied on top of the unjittered clip pos
  vec2 prev_jitter;
} frame;

layout(push_constant) uniform Push {
  mat4 model;
  mat4 prev_model;
} push;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec4 out_curr_clip;
layout(location = 2) out vec4 out_prev_clip;

void main() {
  vec4 clip = frame.view_proj * push.model * vec4(in_position, 1.0);
  // Motion vectors compare unjittered positions, so jitter only moves the
  // rasterized sample, never the reprojection.
  out_curr_clip = clip;
  out_prev_clip = frame.prev_view_proj * push.prev_model * vec4(in_position, 1.0);
  gl_Position = clip + vec4(frame.jitter * clip.w, 0.0, 0.0);
  out_normal = mat3(push.model) * in_normal;
}

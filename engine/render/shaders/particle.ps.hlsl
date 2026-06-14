// Soft, sun-lit billboard particles. A sphere impostor normal gives volumetric
// shading; the prepass depth fades the particle out as it nears geometry (soft
// particles) and clips it where it is fully occluded.
[[vk::binding(1, 0)]] Texture2D<float> scene_depth;  // reversed-z, point-fetched

struct PushData {
  column_major float4x4 view_proj;
  float3 cam_right;
  float near_plane;
  float3 cam_up;
  float soft_fade;
  float3 sun_dir;
  float sun_intensity;
  float3 sun_color;
  float ambient;
};
[[vk::push_constant]] PushData push;

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 color : COLOR0;
};

float4 main(PsIn input) : SV_Target0 {
  float r2 = dot(input.uv, input.uv);
  if (r2 > 1.0) discard;  // round sprite

  // Sphere impostor normal, oriented to the camera basis.
  float3 cam_forward = normalize(cross(push.cam_right, push.cam_up));
  float3 n = normalize(push.cam_right * input.uv.x + push.cam_up * input.uv.y +
                       cam_forward * sqrt(max(1.0 - r2, 0.0)));
  float ndl = saturate(dot(n, normalize(-push.sun_dir)));
  float3 lit = input.color.rgb * (push.ambient + push.sun_color * push.sun_intensity * ndl * 0.25);

  // Soft particles: fade against the opaque depth (linear view z).
  uint w, h;
  scene_depth.GetDimensions(w, h);
  float scene_d = scene_depth.Load(int3(int2(input.pos.xy), 0)).r;
  float scene_vz = push.near_plane / max(scene_d, 1e-6);
  float part_vz = push.near_plane / max(input.pos.z, 1e-6);
  float soft = saturate((scene_vz - part_vz) / max(push.soft_fade, 1e-3));

  float alpha = input.color.a * smoothstep(1.0, 0.55, sqrt(r2)) * soft;
  if (alpha <= 0.001) discard;
  return float4(lit, alpha);
}

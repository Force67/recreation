// Asset-thumbnail vertex stage. Transforms a mesh vertex by a baked MVP (an
// orthographic 3/4 view framing the mesh bounds) and passes the model-space
// normal through for flat clay lighting in the pixel stage. Matches the engine's
// column-major / mul(matrix, vector) convention (see mesh.vs.hlsl).

struct PushData {
  column_major float4x4 mvp;
  float4 albedo;  // rgb clay colour
  float4 light;   // xyz key-light direction (model space)
};
[[vk::push_constant]] PushData push;

struct VsIn {
  [[vk::location(0)]] float3 pos : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
};

struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
};

VsOut main(VsIn input) {
  VsOut output;
  output.sv_position = mul(push.mvp, float4(input.pos, 1.0));
  output.normal = input.normal;
  return output;
}

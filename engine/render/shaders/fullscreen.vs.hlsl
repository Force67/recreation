struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

// Single triangle covering the screen, no vertex buffer.
VsOut main(uint vertex_id : SV_VertexID) {
  VsOut output;
  output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.sv_position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
  return output;
}

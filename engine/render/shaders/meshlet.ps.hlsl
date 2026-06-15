// Tints each meshlet a distinct hashed color so the cluster decomposition is
// directly visible, with a little n.l so the form still reads.
struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] nointerpolation uint meshlet : MESHLET;
};

float3 MeshletColor(uint i) {
  uint h = i * 2654435761u;
  return float3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0;
}

float4 main(PsIn input) : SV_Target0 {
  float3 base = MeshletColor(input.meshlet);
  float ndl = saturate(dot(normalize(input.normal), normalize(float3(0.4, 1.0, 0.3))));
  return float4(base * (0.35 + 0.65 * ndl), 1.0);
}

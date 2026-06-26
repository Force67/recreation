// Overdraw visualization: every fragment adds a warm increment into a cleared
// target with additive blending, so the accumulated brightness ramps
// black -> red -> orange -> yellow -> white with the number of overlapping
// layers. Paired with shadow.vs (which just outputs the camera-projected
// position) and read back through the normal tonemap.
float4 main() : SV_Target0 {
  return float4(0.11, 0.065, 0.03, 0.0);
}

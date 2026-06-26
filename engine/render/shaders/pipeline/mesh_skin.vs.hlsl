// GPU-skinned permutation of mesh.vs.hlsl: adds the bone weight vertex stream
// and blends through the per-frame bone palette. Compiled separately so static
// geometry keeps the lean vertex layout.
#define REC_SKINNED 1
#include "mesh.vs.hlsl"

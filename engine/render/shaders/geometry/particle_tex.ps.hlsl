// Bindless-textured variant of particle.ps: adds the engine's bindless texture
// table (set 1) so a billboard can sample its authored effect texture. Built
// only when the device exposes bindless; the plain particle.ps is the fallback.
#define REC_PARTICLE_BINDLESS 1
#include "particle.ps.hlsl"

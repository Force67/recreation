// Alpha-tested (cutout) variant of the depth/normal/motion prepass. The base
// prepass.ps carries no discard so opaque draws keep early-Z; masked
// submeshes bind this variant instead.
#define REC_PREPASS_MASKED 1
#include "prepass.ps.hlsl"

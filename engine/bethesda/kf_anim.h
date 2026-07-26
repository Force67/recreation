#ifndef RECREATION_BETHESDA_KF_ANIM_H_
#define RECREATION_BETHESDA_KF_ANIM_H_

#include <string_view>

#include "asset/skeleton.h"
#include "core/types.h"

namespace rx::bethesda {

// Gamebryo .kf animation (Fallout 3 / New Vegas). A .kf is a NIF 20.2.0.7 file
// whose root is a NiControllerSequence: a named clip with a start/stop time and
// one controlled block per animated bone, each pointing at an interpolator.
// Two interpolator kinds appear in the shipped clips and both are read here:
//
//   NiTransformInterpolator        - plain keyframe lists (NiTransformData)
//   NiBSplineCompTransformInterp.. - cubic B-spline control points, quantized
//                                    to shorts with a per-channel bias and
//                                    multiplier (the bulk of the animations)
//
// The B-spline curves are sampled onto keyframes at a fixed rate, so the result
// is an ordinary asset::AnimationClip that the existing pose sampler plays with
// no special case. Bone tracks are bound by node name against `skeleton`;
// tracks naming a bone the skeleton lacks are dropped.
//
// This is NOT Havok: Skyrim/FO4 clips are .hkx and go through hkx_anim.h.
bool ConvertKfAnimation(ByteSpan data, asset::AssetId id, const asset::Skeleton& skeleton,
                        asset::AnimationClip* out);

// Samples per second the B-spline curves are baked at. The shipped clips are
// authored at 30fps and the sampler interpolates between keys, so this loses
// nothing perceptible while keeping the clips small.
inline constexpr f32 kKfBakeRate = 30.0f;

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_KF_ANIM_H_

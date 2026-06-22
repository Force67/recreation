#ifndef RECREATION_ANIM_FOOT_IK_H_
#define RECREATION_ANIM_FOOT_IK_H_

#include <functional>

#include "anim/pose.h"
#include "asset/skeleton.h"
#include "core/math.h"

namespace rec::anim {

// Raycast straight down from a model-space origin; on a hit fill the model-space
// ground point and surface normal. The engine wraps the physics world (which
// works in engine/world space) and converts to/from the actor model space.
using GroundQuery = std::function<bool(const Vec3& origin, Vec3* hit, Vec3* normal)>;

// SOTA-ish foot IK for a biped: drops the pelvis so the lower foot can reach,
// then analytic two-bone IK plants each foot on the ground and pitches it to
// the surface normal. Operates on the locomotion pose in model space; rewrites
// the hip/knee/ankle/pelvis local transforms and leaves `bone_model` updated.
//
// `up` and `forward` are the model-space ground-up and character-facing axes
// (engine test rig: +Y / +Z; Skyrim skeleton: +Z / +Y). `ankle_height` is the
// distance from the ankle joint down to the sole. `foot_weight` is the stance
// weight per leg (left, right): 1 plants the foot, 0 leaves it to the swing
// (so a walking foot lifts instead of sticking to the ground).
void SolveFootIk(const asset::Skeleton& skeleton, const GroundQuery& ground, const Vec3& up,
                 const Vec3& forward, f32 ankle_height, const f32 foot_weight[2],
                 SkeletonPose* pose, base::Vector<Mat4>* bone_model);

}  // namespace rec::anim

#endif  // RECREATION_ANIM_FOOT_IK_H_

#ifndef RECREATION_ANIM_LOCOMOTION_H_
#define RECREATION_ANIM_LOCOMOTION_H_

#include "anim/pose.h"
#include "asset/skeleton.h"
#include "core/types.h"

namespace rec::anim {

// Procedural biped locomotion: synthesizes an idle/walk/run gait directly on a
// skeleton, keyed on the standard Skyrim bone names. No clips, no Havok. Foot
// IK (engine/anim/foot_ik) refines the result against the ground afterwards.
//
// `speed` is planar m/s and blends gait amplitude/frequency; `phase` is the
// 0..1 gait cycle position (advance it with AdvancePhase each frame). The pose
// is reset to bind internally, then the driven joints are rotated.
struct Locomotion {
  f32 phase = 0;
  // Tuning, exposed so the same code drives a hand-built test rig and the real
  // Skyrim skeleton (whose bone axes differ); signs flip per skeleton.
  f32 thigh_axis_sign = 1.0f;
  f32 arm_axis_sign = 1.0f;
  bool is_biped = true;  // non-bipeds get a breathing idle only

  void Apply(const asset::Skeleton& skeleton, f32 speed, SkeletonPose* pose) const;
};

// Advance and wrap the 0..1 gait phase for one frame at the given planar speed.
f32 AdvancePhase(f32 phase, f32 speed, f32 dt);

}  // namespace rec::anim

#endif  // RECREATION_ANIM_LOCOMOTION_H_

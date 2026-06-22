#include "anim/locomotion.h"

#include <cmath>

namespace rec::anim {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 2.0f * kPi;
constexpr f32 kStride = 1.5f;  // meters per gait cycle, sets step frequency

f32 Clamp01(f32 v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Layer a local-space rotation delta about `axis` onto a bone's bind pose.
void Swing(const asset::Skeleton& skeleton, SkeletonPose* pose, const char* bone,
           const Vec3& axis, f32 radians) {
  i32 b = skeleton.Find(bone);
  if (b < 0) return;
  pose->rotation[b] = skeleton.bones[b].bind_rotation * QuatFromAxisAngle(axis, radians);
}

}  // namespace

f32 AdvancePhase(f32 phase, f32 speed, f32 dt) {
  f32 cycles_per_second = speed > 0.01f ? speed / kStride : 0.5f;  // idle ticks slowly
  phase += cycles_per_second * dt;
  phase -= std::floor(phase);
  return phase;
}

void Locomotion::Apply(const asset::Skeleton& skeleton, f32 speed, SkeletonPose* pose) const {
  pose->ResetToBind(skeleton);

  // Gait amplitude grows from a tiny idle sway through walk into a big run.
  f32 walk = Clamp01(speed / 1.5f);
  f32 run = Clamp01((speed - 1.5f) / 3.5f);
  f32 theta = phase * kTwoPi;

  if (!is_biped) {
    // Quadrupeds/creatures: a slow breathing bob on the spine, no stepping.
    f32 breathe = std::sin(theta) * 0.03f;
    Swing(skeleton, pose, "NPC Spine [Spn0]", {1, 0, 0}, breathe);
    return;
  }

  f32 leg = std::sin(theta);            // left leg leads, right is opposite phase
  f32 thigh_amp = 0.06f + 0.45f * walk + 0.35f * run;
  f32 knee_amp = 0.10f + 0.7f * walk + 0.6f * run;
  f32 arm_amp = 0.05f + 0.35f * walk + 0.45f * run;

  const Vec3 x{1, 0, 0};
  f32 ts = thigh_axis_sign;
  // Hips swing fore/aft in antiphase; knees bend most as the leg passes under.
  Swing(skeleton, pose, "NPC L Thigh [LThg]", x, ts * thigh_amp * leg);
  Swing(skeleton, pose, "NPC R Thigh [RThg]", x, -ts * thigh_amp * leg);
  Swing(skeleton, pose, "NPC L Calf [LClf]", x, -knee_amp * Clamp01(-std::sin(theta - 0.6f)));
  Swing(skeleton, pose, "NPC R Calf [RClf]", x, -knee_amp * Clamp01(-std::sin(theta + kPi - 0.6f)));

  // Arms counter-swing the legs.
  f32 as = arm_axis_sign;
  Swing(skeleton, pose, "NPC L UpperArm [LUar]", x, -as * arm_amp * leg);
  Swing(skeleton, pose, "NPC R UpperArm [RUar]", x, as * arm_amp * leg);

  // Spine sways side to side once per stride. (A vertical pelvis bob is left to
  // foot IK, which owns the body's height over the ground.)
  Swing(skeleton, pose, "NPC Spine1 [Spn1]", {0, 0, 1}, 0.04f * walk * std::cos(theta));
}

}  // namespace rec::anim

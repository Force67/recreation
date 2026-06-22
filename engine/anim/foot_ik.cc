#include "anim/foot_ik.h"

#include <algorithm>
#include <cmath>

namespace rec::anim {
namespace {

struct Leg {
  const char* hip;
  const char* knee;
  const char* ankle;
};
constexpr Leg kLegs[2] = {
    {"NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]"},
    {"NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]"},
};

f32 Clamp(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Set bone `b`'s parent-relative rotation so its model-space rotation becomes
// `world`. parent_world is the parent bone's model-space rotation.
void SetLocalFromWorld(SkeletonPose* pose, i32 b, const Quat& parent_world, const Quat& world) {
  pose->rotation[b] = Conjugate(parent_world) * world;
}

}  // namespace

void SolveFootIk(const asset::Skeleton& skeleton, const GroundQuery& ground, const Vec3& up,
                 const Vec3& forward, f32 ankle_height, const f32 foot_weight[2],
                 SkeletonPose* pose, base::Vector<Mat4>* bone_model) {
  ComputeModelMatrices(skeleton, *pose, bone_model);

  // 1. Probe the ground under each ankle; record how far each foot must move
  //    along `up` to reach its plant target.
  struct Plant {
    i32 hip = -1, knee = -1, ankle = -1;
    bool grounded = false;
    f32 weight = 1;  // stance weight (1 plant, 0 swing)
    Vec3 target;     // model-space ankle goal
    Vec3 normal{0, 1, 0};
    f32 lift = 0;    // signed distance along `up` from current ankle to target
  };
  Plant plants[2];
  f32 lowest_lift = 0;
  bool any = false;
  for (int i = 0; i < 2; ++i) {
    Plant& p = plants[i];
    p.weight = foot_weight[i];
    p.hip = skeleton.Find(kLegs[i].hip);
    p.knee = skeleton.Find(kLegs[i].knee);
    p.ankle = skeleton.Find(kLegs[i].ankle);
    if (p.hip < 0 || p.knee < 0 || p.ankle < 0) continue;
    Vec3 ankle_pos = Translation((*bone_model)[p.ankle]);
    Vec3 hit, normal;
    if (!ground(ankle_pos + up * 0.5f, &hit, &normal)) continue;
    p.grounded = true;
    p.normal = normal;
    p.target = hit + up * ankle_height;
    p.lift = Dot(p.target - ankle_pos, up);
    // Only planted feet pull the pelvis down (a lifting swing foot must not).
    if (p.weight >= 0.5f) lowest_lift = std::min(lowest_lift, p.lift);
    any = true;
  }
  if (!any) return;

  // 2. Drop the pelvis to the lower foot so the higher foot keeps a bent knee
  //    instead of locking straight (classic pelvis adaptation).
  i32 pelvis = skeleton.Find("NPC Pelvis [Pelv]");
  if (pelvis < 0) pelvis = skeleton.Find("NPC Root [Root]");
  if (pelvis >= 0 && lowest_lift < 0) {
    f32 drop = std::max(lowest_lift, -0.5f);  // clamp so we never fold up
    pose->translation[pelvis] += up * drop;
    ComputeModelMatrices(skeleton, *pose, bone_model);
  }

  // 3. Analytic two-bone IK per leg, knee biased toward `forward`. A swinging
  //    foot (low weight) keeps its locomotion pose so the stride still lifts.
  for (const Plant& p : plants) {
    if (!p.grounded || p.weight < 0.5f) continue;
    Vec3 a = Translation((*bone_model)[p.hip]);
    Vec3 b = Translation((*bone_model)[p.knee]);
    Vec3 c = Translation((*bone_model)[p.ankle]);
    f32 l_thigh = Length(b - a);
    f32 l_calf = Length(c - b);
    if (l_thigh < 1e-4f || l_calf < 1e-4f) continue;

    Vec3 to_target = p.target - a;
    f32 reach = Clamp(Length(to_target), std::fabs(l_thigh - l_calf) + 1e-3f,
                      l_thigh + l_calf - 1e-3f);
    Vec3 dir = Normalize(to_target);
    Vec3 effective_target = a + dir * reach;

    // Knee angle (hip->target vs hip->knee) by the law of cosines, then place
    // the knee in the plane spanned by `dir` and the forward pole.
    f32 cos_hip = Clamp((l_thigh * l_thigh + reach * reach - l_calf * l_calf) /
                            (2.0f * l_thigh * reach),
                        -1.0f, 1.0f);
    f32 hip_angle = std::acos(cos_hip);
    Vec3 plane_n = Cross(dir, forward);
    if (Length(plane_n) < 1e-4f) plane_n = Cross(dir, up);
    plane_n = Normalize(plane_n);
    Vec3 hip_to_knee = Rotate(QuatFromAxisAngle(plane_n, hip_angle), dir);
    Vec3 knee_pos = a + hip_to_knee * l_thigh;

    // Aim the thigh segment along its new direction, then the calf along the
    // remaining direction to the target (twist is preserved from the pose).
    Quat hip_world = QuatFromMat4((*bone_model)[p.hip]);
    Quat knee_world = QuatFromMat4((*bone_model)[p.knee]);
    i32 hip_parent = skeleton.bones[p.hip].parent;
    Quat hip_parent_world =
        hip_parent >= 0 ? QuatFromMat4((*bone_model)[hip_parent]) : Quat{0, 0, 0, 1};

    Quat delta_hip = QuatBetween(b - a, knee_pos - a);
    Quat new_hip_world = delta_hip * hip_world;
    SetLocalFromWorld(pose, p.hip, hip_parent_world, new_hip_world);

    Vec3 calf_dir_after_hip = Rotate(delta_hip, c - b);
    Quat delta_knee = QuatBetween(calf_dir_after_hip, effective_target - knee_pos);
    Quat new_knee_world = delta_knee * delta_hip * knee_world;
    SetLocalFromWorld(pose, p.knee, new_hip_world, new_knee_world);

    // Pitch the foot toward the surface normal so it does not clip the slope.
    if (p.ankle >= 0) {
      Quat ankle_world = QuatFromMat4((*bone_model)[p.ankle]);
      Quat new_ankle_world = QuatBetween(Rotate(delta_knee * delta_hip, up), p.normal) *
                             delta_knee * delta_hip * ankle_world;
      SetLocalFromWorld(pose, p.ankle, new_knee_world, new_ankle_world);
    }
    ComputeModelMatrices(skeleton, *pose, bone_model);
  }
}

}  // namespace rec::anim

#ifndef KINEMA_JOLT_ADAPTER_H_
#define KINEMA_JOLT_ADAPTER_H_

// Header-only bridge between kinema poses and Jolt Physics ragdolls. Include
// this only from translation units that already see Jolt headers; the kinema
// core library stays physics-agnostic.
//
//   JPH::Ref<JPH::Skeleton> jolt_skel = kinema::jolt::MakeSkeleton(skeleton);
//   kinema::jolt::SetRagdollPose(*ragdoll, root, model_t, model_r);   // hard key
//   kinema::jolt::DriveRagdollPose(*ragdoll, root, model_t, model_r); // motors
//
// Model-space arrays come from kinema::ComputeModelSpace. The ragdoll's
// skeleton must have been built with the same bone order (MakeSkeleton).

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>

#include "kinema/kinema.h"

namespace kinema::jolt {

inline JPH::Ref<JPH::Skeleton> MakeSkeleton(const Skeleton& skeleton) {
  JPH::Ref<JPH::Skeleton> out = new JPH::Skeleton();
  char name[32];
  for (u32 i = 0; i < skeleton.count(); ++i) {
    // Jolt keys joints by string name; the hash keeps names stable without
    // hauling the original strings around.
    std::snprintf(name, sizeof(name), "b%016llx",
                  static_cast<unsigned long long>(skeleton.name_hashes[i]));
    out->AddJoint(name, skeleton.parents[i]);
  }
  return out;
}

// Builds a Jolt SkeletonPose in model space from kinema model-space arrays
// (scale is not representable on rigid bodies and is ignored).
inline void FillPose(const Vec3* model_t, const Quat* model_r, u32 count,
                     JPH::SkeletonPose* pose) {
  for (u32 i = 0; i < count && i < pose->GetJointCount(); ++i) {
    JPH::SkeletonPose::JointState& joint = pose->GetJoint(static_cast<int>(i));
    joint.mTranslation = JPH::Vec3(model_t[i].x, model_t[i].y, model_t[i].z);
    joint.mRotation = JPH::Quat(model_r[i].x, model_r[i].y, model_r[i].z, model_r[i].w);
  }
}

// Hard keying: teleports the ragdoll bodies onto the pose (kinematic sync,
// death-blend targets, spawn placement).
inline void SetRagdollPose(JPH::Ragdoll& ragdoll, const JPH::RVec3& root, const Vec3* model_t,
                           const Quat* model_r, u32 count) {
  JPH::SkeletonPose pose;
  pose.SetSkeleton(ragdoll.GetRagdollSettings()->GetSkeleton());
  pose.SetRootOffset(root);
  FillPose(model_t, model_r, count, &pose);
  pose.CalculateJointMatrices();
  ragdoll.SetPose(pose);
}

// Soft keying: sets the constraint motor targets so the ragdoll is *driven*
// toward the animated pose while staying physical (hit reactions, stumbles).
inline void DriveRagdollPose(JPH::Ragdoll& ragdoll, const JPH::RVec3& root, const Vec3* model_t,
                             const Quat* model_r, u32 count) {
  JPH::SkeletonPose pose;
  pose.SetSkeleton(ragdoll.GetRagdollSettings()->GetSkeleton());
  pose.SetRootOffset(root);
  FillPose(model_t, model_r, count, &pose);
  pose.CalculateJointMatrices();
  ragdoll.DriveToPoseUsingMotors(pose);
}

}  // namespace kinema::jolt

#endif  // KINEMA_JOLT_ADAPTER_H_

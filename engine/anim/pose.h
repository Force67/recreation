#ifndef RECREATION_ANIM_POSE_H_
#define RECREATION_ANIM_POSE_H_

#include <base/containers/vector.h>

#include "asset/skeleton.h"
#include "core/math.h"
#include "core/types.h"

namespace rec::anim {

// Per-bone local transform the animation writes each frame, parent-relative.
// Starts at the skeleton bind so untouched bones keep their rest pose.
struct SkeletonPose {
  base::Vector<Vec3> translation;
  base::Vector<Quat> rotation;
  base::Vector<f32> scale;

  void ResetToBind(const asset::Skeleton& skeleton);
  u32 size() const { return static_cast<u32>(rotation.size()); }
};

// Resolve each skin bone name to its index in the skeleton, once per mesh.
// Returns -1 for bones the skeleton lacks (those collapse to the root).
base::Vector<i32> BuildBoneRemap(const asset::Skeleton& skeleton, const asset::SkinBinding& skin);

// Walk the hierarchy front to back (parents precede children) composing
// local transforms into model space matrices, one per skeleton bone.
void ComputeModelMatrices(const asset::Skeleton& skeleton, const SkeletonPose& pose,
                          base::Vector<Mat4>* out_model);

// GPU skinning palette for one skinned mesh: palette[i] = bone_model[remap[i]]
// * inverse_bind[i]. Column major, ready to upload to the bone palette buffer.
void BuildSkinPalette(const base::Vector<Mat4>& bone_model, const asset::SkinBinding& skin,
                      const base::Vector<i32>& remap, base::Vector<Mat4>* out_palette);

}  // namespace rec::anim

#endif  // RECREATION_ANIM_POSE_H_

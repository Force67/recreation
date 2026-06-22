#include "anim/pose.h"

namespace rec::anim {

void SkeletonPose::ResetToBind(const asset::Skeleton& skeleton) {
  u32 count = static_cast<u32>(skeleton.bones.size());
  translation.resize(count);
  rotation.resize(count);
  scale.resize(count);
  for (u32 i = 0; i < count; ++i) {
    translation[i] = skeleton.bones[i].bind_translation;
    rotation[i] = skeleton.bones[i].bind_rotation;
    scale[i] = skeleton.bones[i].bind_scale;
  }
}

base::Vector<i32> BuildBoneRemap(const asset::Skeleton& skeleton,
                                 const asset::SkinBinding& skin) {
  base::Vector<i32> remap;
  remap.reserve(skin.bones.size());
  for (const std::string& name : skin.bones) remap.push_back(skeleton.Find(name));
  return remap;
}

void ComputeModelMatrices(const asset::Skeleton& skeleton, const SkeletonPose& pose,
                          base::Vector<Mat4>* out_model) {
  u32 count = static_cast<u32>(skeleton.bones.size());
  out_model->resize(count);
  for (u32 i = 0; i < count; ++i) {
    Mat4 local = (i < pose.size())
                     ? MakeTransform(pose.translation[i], pose.rotation[i], pose.scale[i])
                     : MakeTransform(skeleton.bones[i].bind_translation,
                                     skeleton.bones[i].bind_rotation, skeleton.bones[i].bind_scale);
    i32 parent = skeleton.bones[i].parent;
    // Parents always precede children (ConvertNifSkeleton orders them), so the
    // parent's model matrix is already final.
    (*out_model)[i] = (parent >= 0 && parent < static_cast<i32>(i)) ? (*out_model)[parent] * local
                                                                    : local;
  }
}

void BuildSkinPalette(const base::Vector<Mat4>& bone_model, const asset::SkinBinding& skin,
                      const base::Vector<i32>& remap, base::Vector<Mat4>* out_palette) {
  u32 count = static_cast<u32>(skin.bones.size());
  out_palette->resize(count);
  for (u32 i = 0; i < count; ++i) {
    i32 b = i < remap.size() ? remap[i] : -1;
    if (b >= 0 && b < static_cast<i32>(bone_model.size())) {
      (*out_palette)[i] = bone_model[b] * skin.inverse_bind[i];
    } else {
      (*out_palette)[i] = Mat4::Identity();
    }
  }
}

}  // namespace rec::anim

#ifndef RECREATION_ASSET_SKELETON_H_
#define RECREATION_ASSET_SKELETON_H_

#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "asset/asset_id.h"
#include "core/math.h"
#include "core/types.h"

namespace rec::asset {

// One joint of a skeleton. The bind pose is the parent-relative rest transform
// in Bethesda object space (Z-up, game units); the per-entity model matrix
// carries the engine space conversion, the same as static NIF geometry. Stored
// decomposed so animation can layer rotation deltas onto the rest orientation.
struct Bone {
  std::string name;
  i32 parent = -1;  // index into Skeleton::bones, -1 = root
  Vec3 bind_translation;
  Quat bind_rotation;
  f32 bind_scale = 1.0f;
};

// A flat bone array (parents always precede children after ConvertNifSkeleton
// orders them) plus name lookup. Built from skeleton.nif or a self-contained
// creature NIF.
struct Skeleton {
  AssetId id;
  base::Vector<Bone> bones;

  i32 Find(std::string_view name) const {
    for (u32 i = 0; i < bones.size(); ++i) {
      if (bones[i].name == name) return static_cast<i32>(i);
    }
    return -1;
  }
};

// Binds one skinned mesh to a skeleton by bone name. `bones[i]` names the bone
// that vertex skin index i refers to (MeshLod::skinning stores these local
// indices). `inverse_bind[i]` maps a bind-pose vertex into that bone's local
// space (Skyrim NiSkinData skin-to-bone). The GPU palette is
// `bone_model[skeleton.Find(bones[i])] * inverse_bind[i]`.
struct SkinBinding {
  base::Vector<std::string> bones;
  base::Vector<Mat4> inverse_bind;

  bool empty() const { return bones.empty(); }
};

// Keyframed clip. v1 fills these procedurally (see engine/anim/locomotion); the
// sampler also serves loaded clips later. Quaternions are (x, y, z, w).
struct RotKey {
  f32 time = 0;
  f32 q[4] = {0, 0, 0, 1};
};
struct PosKey {
  f32 time = 0;
  f32 p[3] = {0, 0, 0};
};
struct BoneTrack {
  i32 bone = -1;
  base::Vector<RotKey> rot;
  base::Vector<PosKey> pos;
};
struct AnimationClip {
  AssetId id;
  f32 duration = 0;
  bool loop = true;
  base::Vector<BoneTrack> tracks;
};

}  // namespace rec::asset

#endif  // RECREATION_ASSET_SKELETON_H_

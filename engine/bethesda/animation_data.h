#ifndef RECREATION_BETHESDA_ANIMATION_DATA_H_
#define RECREATION_BETHESDA_ANIMATION_DATA_H_

// Bethesda's animation data sidecars. Skyrim strips Havok's extracted motion
// and annotation tracks from the shipped .hkx animations; root motion and
// trigger events live in text files instead:
//   meshes/animationdata/<project>.txt            clip blocks: generator name,
//     animation cache index, playback speed, crop, trigger events (Name:time)
//   meshes/animationdata/boundanims/anims_<project>.txt   per animation index:
//     duration + cumulative translation/rotation keys (game units, Z-up)
// The animation cache index is the position of the animation's path in the
// project's hkbCharacterStringData::animationNames (hkx_character.h), which
// is how the blocks tie back to files on disk.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace rec::bethesda {

struct MotionKey {
  f32 time = 0;
  f32 value[4] = {0, 0, 0, 1};  // xyz translation, or xyzw rotation
};

// Cumulative root displacement over the clip: value(t) is the offset from the
// clip's start, not a per-key delta. Keys are sparse (often just the final
// one); (0, identity) at t=0 is implicit.
struct AnimMotion {
  f32 duration = 0;
  std::vector<MotionKey> translation;
  std::vector<MotionKey> rotation;
};

struct ClipEvent {
  std::string name;
  f32 time = 0;  // seconds from clip start
};

struct ClipData {
  std::string name;  // behavior clip generator name ("MT_WalkForward")
  i32 animation_index = -1;
  f32 playback_speed = 1;
  f32 crop_start = 0;
  f32 crop_end = 0;
  std::vector<ClipEvent> events;
};

struct AnimationData {
  std::vector<ClipData> clips;
  std::unordered_map<i32, AnimMotion> motion;  // by animation cache index
};

// Parses a project file + its boundanims sidecar (either may be empty).
AnimationData ParseAnimationData(std::string_view project_text, std::string_view motion_text);

// Piecewise-linear cumulative translation at `time` (clamped to the key range).
Vec3 SampleMotionTranslation(const AnimMotion& motion, f32 time);

// Translation delta from t0 to t1, wrapping through the clip end when t1 < t0
// (looped playback).
Vec3 MotionTranslationDelta(const AnimMotion& motion, f32 t0, f32 t1);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_ANIMATION_DATA_H_

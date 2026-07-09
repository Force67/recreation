#ifndef RECREATION_BETHESDA_HKX_ANIM_H_
#define RECREATION_BETHESDA_HKX_ANIM_H_

// hkaSplineCompressedAnimation decoding + sampling. Skyrim stores every
// animation as per-track NURBS splines over quantized control points
// (positions 8/16-bit range-mapped, rotations 40-bit three-component
// quaternions), chunked into blocks of up to 256 frames. Decode() unpacks
// everything into float control points once; Sample() then evaluates the
// splines (de Boor) at an arbitrary time, yielding parent-space bone
// transforms in the same order as the skeleton when the binding's track map
// is empty (full-body animations), or via track_to_bone otherwise.

#include <optional>
#include <string>
#include <vector>

#include "bethesda/hkx.h"
#include "core/math.h"

namespace rx::bethesda {

struct HkxTrackPose {
  Vec3 translation{};
  f32 rotation[4] = {0, 0, 0, 1};  // x,y,z,w
  f32 scale = 1.0f;
};

struct HkxAnimation {
  f32 duration = 0;
  u32 num_tracks = 0;
  u32 num_frames = 0;
  std::string skeleton_name;       // binding's original skeleton
  std::vector<i16> track_to_bone;  // empty = identity mapping
  bool additive = false;           // binding blendHint: layer over another pose

  // --- decoded spline data (internal layout, stable for Sample) ---
  struct Channel {
    // Static value when has_spline is false; otherwise the NURBS data.
    bool has_spline = false;
    u8 degree = 0;
    std::vector<u8> knots;
    std::vector<f32> control_points;  // stride floats per point
    u32 stride = 0;                   // 3 (vec) or 4 (quat)
    f32 static_value[4] = {0, 0, 0, 1};
  };
  struct Track {
    Channel position;  // stride 3, default (0,0,0)
    Channel rotation;  // stride 4, default identity
    Channel scale;     // stride 3, default (1,1,1)
  };
  struct Block {
    std::vector<Track> tracks;
  };
  std::vector<Block> blocks;
  f32 block_duration = 1.0f;
  f32 frame_duration = 1.0f / 30.0f;
  u32 max_frames_per_block = 256;
};

// Decodes the first spline-compressed animation (+ binding) in the file.
std::optional<HkxAnimation> DecodeAnimation(const HkxFile& hkx);

// Samples all tracks at `time` (clamped to [0, duration]).
void SampleAnimation(const HkxAnimation& animation, f32 time, std::vector<HkxTrackPose>* out);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_HKX_ANIM_H_

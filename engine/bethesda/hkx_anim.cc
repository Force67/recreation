#include "bethesda/hkx_anim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"

namespace rx::bethesda {
namespace {

// hkaSplineCompressedAnimation serialized offsets (hk2010 x64, verified with
// hkxinfo --hex on mt_idle_b_to_a_trans.hkx: duration 4.0 at +0x14, 99
// tracks, 121 frames, block duration 8.5 = 256 * frameDuration).
namespace off {
constexpr u64 kAnimType = 0x10;
constexpr u64 kAnimDuration = 0x14;
constexpr u64 kAnimTracks = 0x18;
constexpr u64 kNumFrames = 0x38;
constexpr u64 kNumBlocks = 0x3C;
constexpr u64 kMaxFramesPerBlock = 0x40;
constexpr u64 kMaskAndQuantSize = 0x44;
constexpr u64 kBlockDuration = 0x48;
constexpr u64 kFrameDuration = 0x50;
constexpr u64 kBlockOffsets = 0x58;   // hkArray<u32>
constexpr u64 kData = 0x98;           // hkArray<u8>
// hkaAnimationBinding
constexpr u64 kBindingSkeletonName = 0x10;
constexpr u64 kBindingAnimation = 0x18;
constexpr u64 kBindingTrackToBone = 0x20;  // hkArray<i16>
constexpr u64 kBindingBlendHint = 0x40;      // 0 normal, 1 additive (hk2010)
constexpr u64 kBindingBlendHint2014 = 0x50;  // after 2014's partitionIndices
}  // namespace off

constexpr int kSplineAnimationType = 3;  // hkaAnimation::TYPE_SPLINE_COMPRESSED (2010)

// Byte cursor over one block's compressed stream.
struct Cursor {
  const u8* data;
  size_t size;
  size_t at = 0;

  u8 U8() { return at < size ? data[at++] : 0; }
  u16 U16() {
    u16 v = 0;
    if (at + 2 <= size) std::memcpy(&v, data + at, 2);
    at += 2;
    return v;
  }
  f32 F32() {
    f32 v = 0;
    if (at + 4 <= size) std::memcpy(&v, data + at, 4);
    at += 4;
    return v;
  }
  void Align(size_t alignment) { at = (at + alignment - 1) & ~(alignment - 1); }
  const u8* Bytes(size_t count) {
    const u8* p = at + count <= size ? data + at : nullptr;
    at += count;
    return p;
  }
};

// Rotation quantization THREECOMP40: 5 bytes = three 12-bit components in
// [-sqrt(1/2), +sqrt(1/2)], 2 bits naming the omitted (largest) component
// and 1 sign bit for it.
void UnpackThreeComp40(const u8* bytes, f32 out[4]) {
  u64 v = 0;
  for (int i = 0; i < 5; ++i) v |= static_cast<u64>(bytes[i]) << (i * 8);
  constexpr f32 kFrac = 1.41421356f / 4094.0f;  // full 12-bit span -> sqrt(2)
  f32 c[3];
  for (int i = 0; i < 3; ++i) {
    u32 raw = static_cast<u32>((v >> (i * 12)) & 0xFFF);
    c[i] = static_cast<f32>(raw) * kFrac - 0.70710678f;
  }
  u32 shift = static_cast<u32>((v >> 36) & 0x3);
  bool invert = ((v >> 38) & 0x1) != 0;
  f32 missing = 1.0f - c[0] * c[0] - c[1] * c[1] - c[2] * c[2];
  missing = missing > 0.0f ? std::sqrt(missing) : 0.0f;
  if (invert) missing = -missing;
  u32 j = 0;
  for (u32 i = 0; i < 4; ++i) out[i] = (i == shift) ? missing : c[j++];
}

// THREECOMP48: 6 bytes = three u16, each carrying a 15-bit component over the
// same [-sqrt(1/2), +sqrt(1/2)] span; the MSBs of items 0/1 name the omitted
// component and the MSB of item 2 is its sign. Verified against HavokLib's
// hk2014 samples (same clip in 40- and 48-bit agrees to quantization error).
void UnpackThreeComp48(const u8* bytes, f32 out[4]) {
  u16 item[3];
  std::memcpy(item, bytes, 6);
  constexpr f32 kFrac = 1.41421356f / 32766.0f;
  f32 c[3];
  for (int i = 0; i < 3; ++i) {
    c[i] = static_cast<f32>(item[i] & 0x7FFF) * kFrac - 0.70710678f;
  }
  u32 shift = static_cast<u32>(((item[0] >> 15) & 1) | ((item[1] >> 14) & 2));
  bool invert = (item[2] >> 15) != 0;
  f32 missing = 1.0f - c[0] * c[0] - c[1] * c[1] - c[2] * c[2];
  missing = missing > 0.0f ? std::sqrt(missing) : 0.0f;
  if (invert) missing = -missing;
  u32 j = 0;
  for (u32 i = 0; i < 4; ++i) out[i] = (i == shift) ? missing : c[j++];
}

// Reads a NURBS header: control point count - 1, degree, then the knot
// vector (bytes).
void ReadSplineHeader(Cursor* cursor, u16* num_items, u8* degree, std::vector<u8>* knots) {
  *num_items = cursor->U16();
  *degree = cursor->U8();
  knots->resize(*num_items + *degree + 2);
  for (auto& k : *knots) k = cursor->U8();
}

// Vector channel (position or scale): per-component static/spline masks in
// one byte (low nibble static, high nibble spline).
void ReadVectorChannel(Cursor* cursor, u8 mask, u8 quant_bits, f32 default_value,
                       HkxAnimation::Channel* out) {
  out->stride = 3;
  out->static_value[0] = out->static_value[1] = out->static_value[2] = default_value;
  u8 spline = (mask >> 4) & 0x7;
  u8 fixed = mask & 0x7;
  spline &= static_cast<u8>(~fixed);  // static wins when both bits are set
  if (spline == 0) {
    for (int c = 0; c < 3; ++c) {
      if (fixed & (1 << c)) out->static_value[c] = cursor->F32();
    }
    return;
  }
  u16 num_items = 0;
  u8 degree = 0;
  ReadSplineHeader(cursor, &num_items, &degree, &out->knots);
  cursor->Align(4);
  f32 range_min[3] = {}, range_max[3] = {};
  f32 fixed_value[3] = {default_value, default_value, default_value};
  // Per component, in x,y,z order: dynamic components carry a min/max pair,
  // static ones a single float.
  for (int c = 0; c < 3; ++c) {
    if (spline & (1 << c)) {
      range_min[c] = cursor->F32();
      range_max[c] = cursor->F32();
    } else if (fixed & (1 << c)) {
      fixed_value[c] = cursor->F32();
    }
  }
  out->has_spline = true;
  out->degree = degree;
  out->control_points.resize(static_cast<size_t>(num_items + 1) * 3);
  const bool sixteen = quant_bits == 1;  // 0 = 8-bit, 1 = 16-bit
  for (u32 p = 0; p <= num_items; ++p) {
    for (int c = 0; c < 3; ++c) {
      f32 value = fixed_value[c];
      if (spline & (1 << c)) {
        f32 t = sixteen ? static_cast<f32>(cursor->U16()) / 65535.0f
                        : static_cast<f32>(cursor->U8()) / 255.0f;
        value = range_min[c] + (range_max[c] - range_min[c]) * t;
      }
      out->control_points[p * 3 + c] = value;
    }
  }
  cursor->Align(4);
}

// Rotation channel: low nibble = static quat present, high nibble = spline.
bool ReadRotationChannel(Cursor* cursor, u8 mask, u8 quant, HkxAnimation::Channel* out) {
  out->stride = 4;
  out->static_value[0] = out->static_value[1] = out->static_value[2] = 0.0f;
  out->static_value[3] = 1.0f;
  // THREECOMP40 (=1) is all Bethesda ships; THREECOMP48 (=2) shows up in
  // HCT-exported mod content. POLAR32/24-bit stay unsupported.
  if (quant != 1 && quant != 2) {
    if ((mask & 0xF0) != 0 || (mask & 0x0F) != 0) return false;
    return true;
  }
  const size_t point_bytes = quant == 1 ? 5 : 6;
  auto unpack = quant == 1 ? UnpackThreeComp40 : UnpackThreeComp48;
  if (mask & 0xF0) {
    u16 num_items = 0;
    u8 degree = 0;
    ReadSplineHeader(cursor, &num_items, &degree, &out->knots);
    // 40-bit quats follow the knots directly; 48-bit payloads align to 2
    // (matching Havok's serializer).
    if (quant == 2) cursor->Align(2);
    out->has_spline = true;
    out->degree = degree;
    out->control_points.resize(static_cast<size_t>(num_items + 1) * 4);
    for (u32 p = 0; p <= num_items; ++p) {
      const u8* bytes = cursor->Bytes(point_bytes);
      if (!bytes) return false;
      unpack(bytes, &out->control_points[p * 4]);
    }
    cursor->Align(4);
  } else if (mask & 0x0F) {
    const u8* bytes = cursor->Bytes(point_bytes);
    if (!bytes) return false;
    unpack(bytes, out->static_value);
    cursor->Align(4);
  }
  return true;
}

// de Boor evaluation of a clamped NURBS with byte knots at parameter u
// (in frame units within the block).
void EvaluateSpline(const HkxAnimation::Channel& channel, f32 u, f32* out) {
  const u32 stride = channel.stride;
  const auto& knots = channel.knots;
  const int degree = channel.degree;
  const int point_count = static_cast<int>(channel.control_points.size() / stride);
  // Find the knot span (clamped).
  int span = degree;
  int max_span = point_count - 1;
  while (span < max_span && static_cast<f32>(knots[span + 1]) <= u) ++span;

  // Working copy of the affected control points.
  f32 work[4 * 4];  // (degree+1) points, degree <= 3
  for (int j = 0; j <= degree; ++j) {
    int idx = span - degree + j;
    idx = std::clamp(idx, 0, point_count - 1);
    for (u32 c = 0; c < stride; ++c) work[j * stride + c] = channel.control_points[idx * stride + c];
  }
  for (int r = 1; r <= degree; ++r) {
    for (int j = degree; j >= r; --j) {
      int i = span - degree + j;
      f32 k0 = static_cast<f32>(knots[std::clamp(i, 0, static_cast<int>(knots.size()) - 1)]);
      f32 k1 = static_cast<f32>(
          knots[std::clamp(i + degree - r + 1, 0, static_cast<int>(knots.size()) - 1)]);
      f32 alpha = k1 > k0 ? (u - k0) / (k1 - k0) : 0.0f;
      alpha = std::clamp(alpha, 0.0f, 1.0f);
      for (u32 c = 0; c < stride; ++c) {
        work[j * stride + c] =
            (1.0f - alpha) * work[(j - 1) * stride + c] + alpha * work[j * stride + c];
      }
    }
  }
  for (u32 c = 0; c < stride; ++c) out[c] = work[degree * stride + c];
}

void SampleChannel(const HkxAnimation::Channel& channel, f32 u, f32* out) {
  if (!channel.has_spline) {
    for (u32 c = 0; c < channel.stride; ++c) out[c] = channel.static_value[c];
    return;
  }
  EvaluateSpline(channel, u, out);
}

}  // namespace

std::optional<HkxAnimation> DecodeAnimation(const HkxFile& hkx) {
  u64 anim_at = HkxFile::kNull;
  for (const HkxObject& obj : hkx.objects()) {
    if (obj.class_name == "hkaSplineCompressedAnimation") {
      anim_at = obj.offset;
      break;
    }
  }
  if (anim_at == HkxFile::kNull) return std::nullopt;

  HkxAnimation animation;
  animation.duration = hkx.F32(anim_at + off::kAnimDuration);
  animation.num_tracks = hkx.U32(anim_at + off::kAnimTracks);
  animation.num_frames = hkx.U32(anim_at + off::kNumFrames);
  animation.max_frames_per_block = hkx.U32(anim_at + off::kMaxFramesPerBlock);
  animation.block_duration = hkx.F32(anim_at + off::kBlockDuration);
  animation.frame_duration = hkx.F32(anim_at + off::kFrameDuration);
  const u32 num_blocks = hkx.U32(anim_at + off::kNumBlocks);
  const u32 mask_bytes = hkx.U32(anim_at + off::kMaskAndQuantSize);

  u32 block_offset_count = 0;
  u64 block_offsets = hkx.Array(anim_at + off::kBlockOffsets, &block_offset_count);
  u32 data_size = 0;
  u64 data = hkx.Array(anim_at + off::kData, &data_size);
  if (data == HkxFile::kNull || block_offset_count < num_blocks || animation.num_tracks == 0) {
    return std::nullopt;
  }

  for (u32 b = 0; b < num_blocks; ++b) {
    u32 begin = hkx.U32(block_offsets + static_cast<u64>(b) * 4);
    u32 end = b + 1 < block_offset_count
                  ? hkx.U32(block_offsets + static_cast<u64>(b + 1) * 4)
                  : data_size;
    if (begin >= data_size || end > data_size || begin >= end) return std::nullopt;

    HkxAnimation::Block block;
    block.tracks.resize(animation.num_tracks);
    Cursor cursor{hkx.data() + data + begin, end - begin};
    // Masks first: 4 bytes per transform track, section padded to
    // maskAndQuantizationSize.
    struct Mask {
      u8 quant, pos, rot, scale;
    };
    std::vector<Mask> masks(animation.num_tracks);
    for (auto& m : masks) {
      m.quant = cursor.U8();
      m.pos = cursor.U8();
      m.rot = cursor.U8();
      m.scale = cursor.U8();
    }
    cursor.at = mask_bytes;  // section is padded; track data starts here

    const bool trace = std::getenv("RX_HKX_TRACE") != nullptr;
    for (u32 t = 0; t < animation.num_tracks; ++t) {
      const Mask& m = masks[t];
      u8 pos_quant = m.quant & 0x3;
      u8 rot_quant = (m.quant >> 2) & 0xF;
      u8 scale_quant = (m.quant >> 6) & 0x3;
      HkxAnimation::Track& track = block.tracks[t];
      if (trace) {
        std::fprintf(stderr, "track %u @ +0x%zx masks q=%02x p=%02x r=%02x s=%02x\n", t,
                     cursor.at, m.quant, m.pos, m.rot, m.scale);
      }
      ReadVectorChannel(&cursor, m.pos, pos_quant, 0.0f, &track.position);
      if (!ReadRotationChannel(&cursor, m.rot, rot_quant, &track.rotation)) {
        RX_WARN("hkx animation: unsupported rotation quantization {}", rot_quant);
        return std::nullopt;
      }
      ReadVectorChannel(&cursor, m.scale, scale_quant, 1.0f, &track.scale);
      cursor.Align(4);
    }
    animation.blocks.push_back(std::move(block));
  }

  // Binding: original skeleton + optional partial-skeleton track map.
  for (const HkxObject& obj : hkx.objects()) {
    if (obj.class_name != "hkaAnimationBinding") continue;
    if (hkx.Pointer(obj.offset + off::kBindingAnimation) != anim_at) continue;
    animation.skeleton_name = std::string(hkx.CString(obj.offset + off::kBindingSkeletonName));
    u32 map_count = 0;
    u64 map = hkx.Array(obj.offset + off::kBindingTrackToBone, &map_count);
    for (u32 i = 0; i < map_count && map != HkxFile::kNull; ++i) {
      animation.track_to_bone.push_back(hkx.I16(map + static_cast<u64>(i) * 2));
    }
    // hk2014 (Fallout 4) inserted a partitionIndices array before blendHint.
    const bool hk2014 = hkx.content_version().compare(0, 7, "hk_2014") == 0;
    animation.additive =
        hkx.U8(obj.offset + (hk2014 ? off::kBindingBlendHint2014 : off::kBindingBlendHint)) != 0;
    break;
  }
  return animation;
}

void SampleAnimation(const HkxAnimation& animation, f32 time, std::vector<HkxTrackPose>* out) {
  out->assign(animation.num_tracks, {});
  if (animation.blocks.empty()) return;
  time = std::clamp(time, 0.0f, animation.duration);
  u32 block_index = animation.block_duration > 0.0f
                        ? static_cast<u32>(time / animation.block_duration)
                        : 0;
  block_index = std::min(block_index, static_cast<u32>(animation.blocks.size()) - 1);
  f32 local = time - static_cast<f32>(block_index) * animation.block_duration;
  f32 u = animation.frame_duration > 0.0f ? local / animation.frame_duration : 0.0f;
  // Clamp to this block's frame range (the last block is short).
  f32 max_u = static_cast<f32>(animation.num_frames - 1 -
                               block_index * (animation.max_frames_per_block - 1));
  u = std::clamp(u, 0.0f, std::max(max_u, 0.0f));

  const HkxAnimation::Block& block = animation.blocks[block_index];
  for (u32 t = 0; t < animation.num_tracks && t < block.tracks.size(); ++t) {
    const HkxAnimation::Track& track = block.tracks[t];
    HkxTrackPose& pose = (*out)[t];
    f32 pos[3], rot[4], scale[3];
    SampleChannel(track.position, u, pos);
    SampleChannel(track.rotation, u, rot);
    SampleChannel(track.scale, u, scale);
    pose.translation = {pos[0], pos[1], pos[2]};
    f32 len = std::sqrt(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3]);
    if (len > 1e-6f) {
      for (int c = 0; c < 4; ++c) pose.rotation[c] = rot[c] / len;
    }
    pose.scale = (scale[0] + scale[1] + scale[2]) / 3.0f;
  }
}

}  // namespace rx::bethesda

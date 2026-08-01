#include "components/bethesda/kf_anim.h"

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <cmath>
#include <cstring>

#include "components/bethesda/nif.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

// Cursor over one block's bytes. Reads past the end are clamped and latch
// `ok`, so a malformed clip degrades to "no tracks" instead of reading wild.
struct Reader {
  ByteSpan data;
  size_t offset = 0;
  bool ok = true;

  template <typename T>
  T Read() {
    T value{};
    if (offset + sizeof(T) > data.size()) {
      ok = false;
      return value;
    }
    std::memcpy(&value, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
  }
  void Skip(size_t bytes) {
    if (offset + bytes > data.size()) {
      ok = false;
      offset = data.size();
      return;
    }
    offset += bytes;
  }
};

// A channel that is not animated stores this offset and leaves its bias and
// multiplier at FLT_MAX; the interpolator's static value stands instead.
constexpr u32 kNoOffset = 0xffffu;

struct BSplineData {
  base::Vector<f32> floats;
  base::Vector<i16> shorts;
};

struct TransformKeys {
  base::Vector<asset::RotKey> rot;
  base::Vector<asset::PosKey> pos;
};

// NiKeyframeData / NiTransformData: three independent key arrays (rotation,
// translation, scale) each with their own interpolation type.
bool ReadTransformData(Reader& r, TransformKeys* out) {
  u32 rot_count = r.Read<u32>();
  if (!r.ok || rot_count > (1u << 20))
    return false;
  u32 rot_type = rot_count > 0 ? r.Read<u32>() : 1;
  if (rot_type == 4) {
    // XYZ rotations: three separate float curves rather than quaternion keys.
    // Rare in the shipped clips; skip the keys and leave rotation static.
    r.Skip(4);  // unused float
    for (int axis = 0; axis < 3; ++axis) {
      u32 count = r.Read<u32>();
      if (!r.ok || count > (1u << 20))
        return false;
      u32 type = count > 0 ? r.Read<u32>() : 1;
      for (u32 i = 0; i < count; ++i) {
        r.Skip(8);  // time + value
        if (type == 2)
          r.Skip(8);  // quadratic tangents
        else if (type == 3)
          r.Skip(12);  // tbc
      }
    }
  } else {
    out->rot.reserve(rot_count);
    for (u32 i = 0; i < rot_count; ++i) {
      asset::RotKey key;
      key.time = r.Read<f32>();
      // NIF stores quaternions w-first; the engine wants (x, y, z, w).
      f32 w = r.Read<f32>();
      key.q[0] = r.Read<f32>();
      key.q[1] = r.Read<f32>();
      key.q[2] = r.Read<f32>();
      key.q[3] = w;
      if (rot_type == 3)
        r.Skip(12);  // tbc
      if (!r.ok)
        return false;
      out->rot.push_back(key);
    }
  }

  u32 pos_count = r.Read<u32>();
  if (!r.ok || pos_count > (1u << 20))
    return false;
  u32 pos_type = pos_count > 0 ? r.Read<u32>() : 1;
  out->pos.reserve(pos_count);
  for (u32 i = 0; i < pos_count; ++i) {
    asset::PosKey key;
    key.time = r.Read<f32>();
    key.p[0] = r.Read<f32>();
    key.p[1] = r.Read<f32>();
    key.p[2] = r.Read<f32>();
    if (pos_type == 2)
      r.Skip(24);  // in/out tangents
    else if (pos_type == 3)
      r.Skip(12);  // tbc
    if (!r.ok)
      return false;
    out->pos.push_back(key);
  }
  // Scale keys follow; the engine's pose has uniform scale it does not animate.
  return r.ok;
}

// One controlled bone: which interpolator drives it and what it is called.
struct ControlledBlock {
  i32 interpolator = -1;
  i32 node_name = -1;  // header string table index
};

// NiBSplineCompTransformInterpolator: a static transform plus, per channel, an
// offset into the shared short control-point pool and a bias/multiplier to
// dequantize with. kNoOffset means the channel is not animated and the static
// value stands.
struct BSplineInterp {
  f32 start_time = 0;
  f32 stop_time = 0;
  i32 spline_data = -1;
  i32 basis_data = -1;
  f32 translation[3] = {0, 0, 0};
  f32 rotation[4] = {0, 0, 0, 1};  // (x, y, z, w)
  f32 scale = 1.0f;
  u32 translation_offset = kNoOffset;
  u32 rotation_offset = kNoOffset;
  u32 scale_offset = kNoOffset;
  f32 translation_bias = 0, translation_multiplier = 0;
  f32 rotation_bias = 0, rotation_multiplier = 0;
  f32 scale_bias = 0, scale_multiplier = 0;
};

// Uniform cubic B-spline basis at parameter u within one segment.
void CubicBasis(f32 u, f32 out[4]) {
  f32 u2 = u * u;
  f32 u3 = u2 * u;
  out[0] = (-u3 + 3.0f * u2 - 3.0f * u + 1.0f) / 6.0f;
  out[1] = (3.0f * u3 - 6.0f * u2 + 4.0f) / 6.0f;
  out[2] = (-3.0f * u3 + 3.0f * u2 + 3.0f * u + 1.0f) / 6.0f;
  out[3] = u3 / 6.0f;
}

// Evaluates one `components`-wide quantized B-spline channel at normalized time
// t in [0, 1]. The control points are interleaved per component and stored as
// shorts scaled by (multiplier, bias).
void EvalBSpline(const base::Vector<i16>& points,
                 u32 offset,
                 u32 control_count,
                 u32 components,
                 f32 bias,
                 f32 multiplier,
                 f32 t,
                 f32* out) {
  for (u32 c = 0; c < components; ++c)
    out[c] = 0;
  if (control_count < 4)
    return;
  u32 segments = control_count - 3;
  f32 scaled = t * static_cast<f32>(segments);
  u32 segment = static_cast<u32>(scaled);
  if (segment >= segments)
    segment = segments - 1;
  f32 u = scaled - static_cast<f32>(segment);
  f32 basis[4];
  CubicBasis(u, basis);
  for (u32 k = 0; k < 4; ++k) {
    u32 index = offset + (segment + k) * components;
    for (u32 c = 0; c < components; ++c) {
      if (index + c >= points.size())
        return;
      f32 value = static_cast<f32>(points[index + c]) / 32767.0f * multiplier + bias;
      out[c] += basis[k] * value;
    }
  }
}

void Normalize(f32 q[4]) {
  f32 len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (len < 1e-6f) {
    q[0] = q[1] = q[2] = 0;
    q[3] = 1;
    return;
  }
  for (int i = 0; i < 4; ++i)
    q[i] /= len;
}

}  // namespace

bool ConvertKfAnimation(ByteSpan data,
                        asset::AssetId id,
                        const asset::Skeleton& skeleton,
                        asset::AnimationClip* out) {
  auto header = ParseNifHeader(data);
  if (!header)
    return false;
  u32 block_count = static_cast<u32>(header->block_sizes.size());
  if (block_count == 0)
    return false;

  auto block_type = [&](u32 index) -> const base::String& {
    return header->block_types[header->block_type_index[index]];
  };
  auto block_reader = [&](u32 index) {
    return Reader{data.subspan(header->block_offsets[index], header->block_sizes[index])};
  };
  auto string_at = [&](i32 index) -> base::String {
    if (index < 0 || static_cast<u32>(index) >= header->strings.size())
      return {};
    return header->strings[index];
  };

  // Pass one: the curve payloads, indexed by block.
  base::UnorderedMap<u32, BSplineData> spline_data;
  base::UnorderedMap<u32, u32> basis_counts;
  base::UnorderedMap<u32, TransformKeys> transform_data;
  for (u32 i = 0; i < block_count; ++i) {
    const base::String& type = block_type(i);
    if (type == "NiBSplineData") {
      Reader r = block_reader(i);
      BSplineData out_data;
      u32 float_count = r.Read<u32>();
      if (!r.ok || float_count > (1u << 22))
        continue;
      out_data.floats.reserve(float_count);
      for (u32 f = 0; f < float_count && r.ok; ++f)
        out_data.floats.push_back(r.Read<f32>());
      u32 short_count = r.Read<u32>();
      if (!r.ok || short_count > (1u << 24))
        continue;
      out_data.shorts.reserve(short_count);
      for (u32 s = 0; s < short_count && r.ok; ++s)
        out_data.shorts.push_back(r.Read<i16>());
      if (r.ok)
        spline_data.emplace(i, base::move(out_data));
    } else if (type == "NiBSplineBasisData") {
      Reader r = block_reader(i);
      u32 count = r.Read<u32>();
      if (r.ok)
        basis_counts.emplace(i, count);
    } else if (type == "NiTransformData" || type == "NiKeyframeData") {
      Reader r = block_reader(i);
      TransformKeys keys;
      if (ReadTransformData(r, &keys))
        transform_data.emplace(i, base::move(keys));
    }
  }

  // Pass two: interpolators.
  base::UnorderedMap<u32, BSplineInterp> bspline_interps;
  base::UnorderedMap<u32, i32> transform_interps;  // block -> NiTransformData block
  for (u32 i = 0; i < block_count; ++i) {
    const base::String& type = block_type(i);
    if (type == "NiBSplineCompTransformInterpolator") {
      Reader r = block_reader(i);
      BSplineInterp interp;
      interp.start_time = r.Read<f32>();
      interp.stop_time = r.Read<f32>();
      interp.spline_data = r.Read<i32>();
      interp.basis_data = r.Read<i32>();
      for (f32& v : interp.translation)
        v = r.Read<f32>();
      // Quaternion is w-first on disk.
      f32 w = r.Read<f32>();
      interp.rotation[0] = r.Read<f32>();
      interp.rotation[1] = r.Read<f32>();
      interp.rotation[2] = r.Read<f32>();
      interp.rotation[3] = w;
      interp.scale = r.Read<f32>();
      interp.translation_offset = r.Read<u32>();
      interp.rotation_offset = r.Read<u32>();
      interp.scale_offset = r.Read<u32>();
      interp.translation_bias = r.Read<f32>();
      interp.translation_multiplier = r.Read<f32>();
      interp.rotation_bias = r.Read<f32>();
      interp.rotation_multiplier = r.Read<f32>();
      interp.scale_bias = r.Read<f32>();
      interp.scale_multiplier = r.Read<f32>();
      if (r.ok)
        bspline_interps.emplace(i, interp);
    } else if (type == "NiTransformInterpolator") {
      Reader r = block_reader(i);
      r.Skip(12);  // static translation
      r.Skip(16);  // static rotation
      r.Skip(4);   // static scale
      i32 keys = r.Read<i32>();
      if (r.ok)
        transform_interps.emplace(i, keys);
    }
  }

  // Pass three: the sequence itself.
  i32 sequence = -1;
  for (u32 i = 0; i < block_count; ++i) {
    if (block_type(i) == "NiControllerSequence") {
      sequence = static_cast<i32>(i);
      break;
    }
  }
  if (sequence < 0)
    return false;

  Reader r = block_reader(static_cast<u32>(sequence));
  i32 name_index = r.Read<i32>();
  u32 controlled_count = r.Read<u32>();
  r.Skip(4);  // array grow by
  if (!r.ok || controlled_count > 4096)
    return false;
  base::Vector<ControlledBlock> controlled;
  controlled.reserve(controlled_count);
  for (u32 i = 0; i < controlled_count; ++i) {
    ControlledBlock block;
    block.interpolator = r.Read<i32>();
    r.Skip(4);  // controller ref
    r.Skip(1);  // priority
    block.node_name = r.Read<i32>();
    r.Skip(4);  // property type
    r.Skip(4);  // controller type
    r.Skip(4);  // controller id
    r.Skip(4);  // interpolator id
    if (!r.ok)
      return false;
    controlled.push_back(block);
  }
  r.Skip(4);  // weight
  r.Skip(4);  // text keys ref
  u32 cycle_type = r.Read<u32>();
  r.Skip(4);  // frequency
  f32 start_time = r.Read<f32>();
  f32 stop_time = r.Read<f32>();
  if (!r.ok)
    return false;

  f32 duration = stop_time - start_time;
  if (!(duration > 0) || duration > 3600.0f)
    return false;

  out->id = id;
  out->duration = duration;
  out->loop = cycle_type == 0;  // 0 = loop, 1 = reverse, 2 = clamp
  out->tracks.clear();

  u32 sample_count = static_cast<u32>(duration * kKfBakeRate) + 1;
  if (sample_count < 2)
    sample_count = 2;
  if (sample_count > 4096)
    sample_count = 4096;

  for (const ControlledBlock& block : controlled) {
    base::String bone_name = string_at(block.node_name);
    if (bone_name.empty())
      continue;
    i32 bone = skeleton.Find(bone_name);
    if (bone < 0)
      continue;  // the clip drives a bone this skeleton lacks
    if (block.interpolator < 0)
      continue;
    u32 interp_block = static_cast<u32>(block.interpolator);

    asset::BoneTrack track;
    track.bone = bone;

    if (const BSplineInterp* interp = bspline_interps.find(interp_block)) {
      const BSplineData* curve = interp->spline_data >= 0
                                     ? spline_data.find(static_cast<u32>(interp->spline_data))
                                     : nullptr;
      const u32* control_count = interp->basis_data >= 0
                                     ? basis_counts.find(static_cast<u32>(interp->basis_data))
                                     : nullptr;
      if (!curve || !control_count)
        continue;
      bool has_rot = interp->rotation_offset != kNoOffset;
      bool has_pos = interp->translation_offset != kNoOffset;
      if (!has_rot && !has_pos)
        continue;
      for (u32 s = 0; s < sample_count; ++s) {
        f32 t = static_cast<f32>(s) / static_cast<f32>(sample_count - 1);
        if (has_rot) {
          asset::RotKey key;
          key.time = t * duration;
          f32 q[4] = {0, 0, 0, 0};
          // Control points are stored w-first like the static quaternion.
          EvalBSpline(curve->shorts, interp->rotation_offset, *control_count, 4,
                      interp->rotation_bias, interp->rotation_multiplier, t, q);
          key.q[0] = q[1];
          key.q[1] = q[2];
          key.q[2] = q[3];
          key.q[3] = q[0];
          Normalize(key.q);
          track.rot.push_back(key);
        }
        if (has_pos) {
          asset::PosKey key;
          key.time = t * duration;
          EvalBSpline(curve->shorts, interp->translation_offset, *control_count, 3,
                      interp->translation_bias, interp->translation_multiplier, t, key.p);
          track.pos.push_back(key);
        }
      }
    } else if (const i32* keys_block = transform_interps.find(interp_block)) {
      const TransformKeys* keys =
          *keys_block >= 0 ? transform_data.find(static_cast<u32>(*keys_block)) : nullptr;
      if (!keys)
        continue;
      track.rot = keys->rot;
      track.pos = keys->pos;
      // Keys are absolute clip times; rebase so the track starts at zero.
      for (asset::RotKey& key : track.rot)
        key.time -= start_time;
      for (asset::PosKey& key : track.pos)
        key.time -= start_time;
    } else {
      continue;
    }

    // The accumulation root carries the cycle's travel (a walk clip translates
    // Bip01 the full stride length). That is root MOTION, not pose: applying it
    // would fling the actor around while the engine separately owns where it
    // stands, so keep the rotation and drop the translation to animate in place.
    if (bone_name == "Bip01" || bone_name == "Bip01 NonAccum")
      track.pos.clear();

    if (track.rot.empty() && track.pos.empty())
      continue;
    out->tracks.push_back(base::move(track));
  }

  if (out->tracks.empty()) {
    RX_WARN("kf {}: no tracks bound to the skeleton", string_at(name_index));
    return false;
  }
  return true;
}

}  // namespace rx::bethesda

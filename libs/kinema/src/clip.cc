#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

#include "kinema/kinema.h"

namespace kinema {
namespace {

constexpr u32 kMagic = 0x314D4E4Bu;  // 'KNM1'
constexpr u32 kVersion = 1;
constexpr u32 kFlagAdditive = 1u << 0;

// All offsets are from the blob start; every block is 8-aligned so the blob
// can be mapped and used in place on any platform we care about.
struct Header {
  u32 magic, version;
  u32 num_tracks, num_frames;
  f32 frame_rate, duration;
  u32 flags;
  u32 num_anim_rot, num_anim_trans, num_anim_scale;
  u32 num_events, num_root_keys, pad0;
  u64 off_const_t, off_const_r, off_const_s;
  u64 off_rot_idx, off_trans_idx, off_scale_idx;
  u64 off_trans_range, off_scale_range;
  u64 off_rot_keys, off_trans_keys, off_scale_keys;
  u64 off_root_keys, off_events, off_strings;
  u64 total_size;
};

struct EventRecord {
  u64 hash;
  f32 time;
  u32 name_off;  // into the string block
};

struct RootKey {
  f32 time, x, y, z;
};

const Header& Head(const u8* blob) { return *reinterpret_cast<const Header*>(blob); }

template <typename T>
const T* Block(const u8* blob, u64 offset) {
  return reinterpret_cast<const T*>(blob + offset);
}

inline f32 Dequant(f32 lerped) { return lerped * (2.0f / 65535.0f) - 1.0f; }

inline u16 QuantSigned(f32 v) {
  f32 q = (std::clamp(v, -1.0f, 1.0f) + 1.0f) * (65535.0f / 2.0f);
  return static_cast<u16>(q + 0.5f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Clip view

std::optional<Clip> Clip::FromBlob(const u8* data, size_t size) {
  if (!data || size < sizeof(Header)) return std::nullopt;
  const Header& h = Head(data);
  if (h.magic != kMagic || h.version != kVersion) return std::nullopt;
  if (h.total_size > size) return std::nullopt;
  if (h.num_anim_rot > h.num_tracks || h.num_anim_trans > h.num_tracks ||
      h.num_anim_scale > h.num_tracks) {
    return std::nullopt;
  }
  Clip clip;
  clip.blob_ = data;
  return clip;
}

u32 Clip::num_tracks() const { return Head(blob_).num_tracks; }
u32 Clip::num_frames() const { return Head(blob_).num_frames; }
f32 Clip::frame_rate() const { return Head(blob_).frame_rate; }
f32 Clip::duration() const { return Head(blob_).duration; }
bool Clip::additive() const { return (Head(blob_).flags & kFlagAdditive) != 0; }
size_t Clip::blob_size() const { return Head(blob_).total_size; }
u32 Clip::num_events() const { return Head(blob_).num_events; }

void Clip::Sample(f32 time, PoseView out) const {
  const Header& h = Head(blob_);
  assert(out.count == h.num_tracks);
  // Constant tracks (and the constant components of everything): one memcpy
  // per channel; the animated tracks overwrite their slots below.
  std::memcpy(out.translation, Block<Vec3>(blob_, h.off_const_t), h.num_tracks * sizeof(Vec3));
  std::memcpy(out.rotation, Block<Quat>(blob_, h.off_const_r), h.num_tracks * sizeof(Quat));
  std::memcpy(out.scale, Block<f32>(blob_, h.off_const_s), h.num_tracks * sizeof(f32));
  if (h.num_frames < 2) return;

  f32 x = std::clamp(time, 0.0f, h.duration) * h.frame_rate;
  u32 k = std::min(static_cast<u32>(x), h.num_frames - 2);
  f32 a = std::clamp(x - static_cast<f32>(k), 0.0f, 1.0f);

  // Rotations: frame-major rows, 4 u16 per animated track.
  if (h.num_anim_rot) {
    const u32 row = h.num_anim_rot * 4;
    const u16* r0 = Block<u16>(blob_, h.off_rot_keys) + static_cast<size_t>(k) * row;
    const u16* r1 = r0 + row;
    const u16* idx = Block<u16>(blob_, h.off_rot_idx);
    for (u32 i = 0; i < h.num_anim_rot; ++i) {
      const u32 base = i * 4;
      f32 c[4];
      for (u32 j = 0; j < 4; ++j) {
        f32 f0 = static_cast<f32>(r0[base + j]);
        f32 f1 = static_cast<f32>(r1[base + j]);
        c[j] = Dequant(f0 + (f1 - f0) * a);
      }
      f32 len2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3];
      f32 inv = len2 > 1e-12f ? 1.0f / std::sqrt(len2) : 0.0f;
      out.rotation[idx[i]] = Quat{c[0] * inv, c[1] * inv, c[2] * inv, c[3] * inv};
    }
  }
  // Translations: 3 u16 per animated track + per-track (min, step) ranges.
  if (h.num_anim_trans) {
    const u32 row = h.num_anim_trans * 3;
    const u16* r0 = Block<u16>(blob_, h.off_trans_keys) + static_cast<size_t>(k) * row;
    const u16* r1 = r0 + row;
    const u16* idx = Block<u16>(blob_, h.off_trans_idx);
    const f32* range = Block<f32>(blob_, h.off_trans_range);  // [track][6]: min3, step3
    for (u32 i = 0; i < h.num_anim_trans; ++i) {
      const u32 base = i * 3;
      const f32* rg = range + static_cast<size_t>(i) * 6;
      f32 v[3];
      for (u32 j = 0; j < 3; ++j) {
        f32 f0 = static_cast<f32>(r0[base + j]);
        f32 f1 = static_cast<f32>(r1[base + j]);
        v[j] = rg[j] + (f0 + (f1 - f0) * a) * rg[3 + j];
      }
      out.translation[idx[i]] = Vec3{v[0], v[1], v[2]};
    }
  }
  // Uniform scales: 1 u16 per animated track.
  if (h.num_anim_scale) {
    const u32 row = h.num_anim_scale;
    const u16* r0 = Block<u16>(blob_, h.off_scale_keys) + static_cast<size_t>(k) * row;
    const u16* r1 = r0 + row;
    const u16* idx = Block<u16>(blob_, h.off_scale_idx);
    const f32* range = Block<f32>(blob_, h.off_scale_range);  // [track][2]: min, step
    for (u32 i = 0; i < h.num_anim_scale; ++i) {
      f32 f0 = static_cast<f32>(r0[i]);
      f32 f1 = static_cast<f32>(r1[i]);
      out.scale[idx[i]] = range[i * 2] + (f0 + (f1 - f0) * a) * range[i * 2 + 1];
    }
  }
}

Vec3 Clip::RootTranslation(f32 time) const {
  const Header& h = Head(blob_);
  const RootKey* keys = Block<RootKey>(blob_, h.off_root_keys);
  Vec3 prev{};
  f32 prev_t = 0;
  for (u32 i = 0; i < h.num_root_keys; ++i) {
    Vec3 v{keys[i].x, keys[i].y, keys[i].z};
    if (time <= keys[i].time) {
      f32 span = keys[i].time - prev_t;
      f32 a = span > 1e-6f ? (time - prev_t) / span : 1.0f;
      return Vec3{prev.x + (v.x - prev.x) * a, prev.y + (v.y - prev.y) * a,
                  prev.z + (v.z - prev.z) * a};
    }
    prev = v;
    prev_t = keys[i].time;
  }
  return prev;
}

Vec3 Clip::RootDelta(f32 t0, f32 t1) const {
  if (t1 >= t0) {
    Vec3 a = RootTranslation(t0), b = RootTranslation(t1);
    return Vec3{b.x - a.x, b.y - a.y, b.z - a.z};
  }
  Vec3 at0 = RootTranslation(t0);
  Vec3 end = RootTranslation(duration());
  Vec3 at1 = RootTranslation(t1);
  return Vec3{end.x - at0.x + at1.x, end.y - at0.y + at1.y, end.z - at0.z + at1.z};
}

ClipEvent Clip::Event(u32 index) const {
  const Header& h = Head(blob_);
  const EventRecord& r = Block<EventRecord>(blob_, h.off_events)[index];
  return ClipEvent{r.hash, r.time,
                   reinterpret_cast<const char*>(blob_ + h.off_strings + r.name_off)};
}

OwnedClip::OwnedClip(std::vector<u8> blob) : blob_(std::move(blob)) {
  clip_ = Clip::FromBlob(blob_.data(), blob_.size());
}

// ---------------------------------------------------------------------------
// Builder

ClipBuilder::ClipBuilder(u32 num_tracks, u32 num_frames, f32 frame_rate)
    : tracks_(num_tracks), frames_(std::max(num_frames, 1u)), rate_(frame_rate) {
  t_.resize(static_cast<size_t>(tracks_) * frames_);
  r_.resize(static_cast<size_t>(tracks_) * frames_);
  s_.assign(static_cast<size_t>(tracks_) * frames_, 1.0f);
}

void ClipBuilder::SetSample(u32 frame, u32 track, const Vec3& t, const Quat& r, f32 s) {
  size_t at = static_cast<size_t>(frame) * tracks_ + track;
  t_[at] = t;
  r_[at] = r;
  s_[at] = s;
}

void ClipBuilder::AddEvent(std::string_view name, f32 time) {
  events_.emplace_back(std::string(name), time);
}

void ClipBuilder::AddRootKey(f32 time, const Vec3& translation) {
  root_keys_.emplace_back(time, translation);
}

std::vector<u8> ClipBuilder::Build() const {
  // Hemisphere-align rotations along each track so quantized components lerp
  // through the short arc, then classify constant vs animated tracks.
  std::vector<Quat> rots = r_;
  for (u32 t = 0; t < tracks_; ++t) {
    for (u32 f = 1; f < frames_; ++f) {
      Quat& q = rots[static_cast<size_t>(f) * tracks_ + t];
      const Quat& p = rots[static_cast<size_t>(f - 1) * tracks_ + t];
      if (q.x * p.x + q.y * p.y + q.z * p.z + q.w * p.w < 0) {
        q = Quat{-q.x, -q.y, -q.z, -q.w};
      }
    }
  }

  // A track is constant when quantizing it could not represent it better than
  // its first value (range below one quantization step).
  constexpr f32 kRotEps = 1.0f / 65535.0f;
  std::vector<u16> anim_rot, anim_trans, anim_scale;
  std::vector<f32> trans_range;  // [animated][6]
  std::vector<f32> scale_range;  // [animated][2]
  for (u32 t = 0; t < tracks_; ++t) {
    f32 tmin[3] = {1e30f, 1e30f, 1e30f}, tmax[3] = {-1e30f, -1e30f, -1e30f};
    f32 smin = 1e30f, smax = -1e30f;
    f32 rdev = 0;
    const Quat& q0 = rots[t];
    for (u32 f = 0; f < frames_; ++f) {
      size_t at = static_cast<size_t>(f) * tracks_ + t;
      const Vec3& v = t_[at];
      tmin[0] = std::min(tmin[0], v.x), tmax[0] = std::max(tmax[0], v.x);
      tmin[1] = std::min(tmin[1], v.y), tmax[1] = std::max(tmax[1], v.y);
      tmin[2] = std::min(tmin[2], v.z), tmax[2] = std::max(tmax[2], v.z);
      smin = std::min(smin, s_[at]), smax = std::max(smax, s_[at]);
      const Quat& q = rots[at];
      rdev = std::max({rdev, std::abs(q.x - q0.x), std::abs(q.y - q0.y), std::abs(q.z - q0.z),
                       std::abs(q.w - q0.w)});
    }
    if (frames_ > 1 && rdev > kRotEps) anim_rot.push_back(static_cast<u16>(t));
    bool t_anim = frames_ > 1 && (tmax[0] - tmin[0] > 1e-5f || tmax[1] - tmin[1] > 1e-5f ||
                                  tmax[2] - tmin[2] > 1e-5f);
    if (t_anim) {
      anim_trans.push_back(static_cast<u16>(t));
      for (int c = 0; c < 3; ++c) trans_range.push_back(tmin[c]);
      for (int c = 0; c < 3; ++c) trans_range.push_back((tmax[c] - tmin[c]) / 65535.0f);
    }
    if (frames_ > 1 && smax - smin > 1e-6f) {
      anim_scale.push_back(static_cast<u16>(t));
      scale_range.push_back(smin);
      scale_range.push_back((smax - smin) / 65535.0f);
    }
  }

  // Quantized frame-major key rows.
  std::vector<u16> rot_keys(static_cast<size_t>(frames_) * anim_rot.size() * 4);
  std::vector<u16> trans_keys(static_cast<size_t>(frames_) * anim_trans.size() * 3);
  std::vector<u16> scale_keys(static_cast<size_t>(frames_) * anim_scale.size());
  for (u32 f = 0; f < frames_; ++f) {
    for (size_t i = 0; i < anim_rot.size(); ++i) {
      const Quat& q = rots[static_cast<size_t>(f) * tracks_ + anim_rot[i]];
      u16* out = &rot_keys[(static_cast<size_t>(f) * anim_rot.size() + i) * 4];
      out[0] = QuantSigned(q.x), out[1] = QuantSigned(q.y);
      out[2] = QuantSigned(q.z), out[3] = QuantSigned(q.w);
    }
    for (size_t i = 0; i < anim_trans.size(); ++i) {
      const Vec3& v = t_[static_cast<size_t>(f) * tracks_ + anim_trans[i]];
      const f32* rg = &trans_range[i * 6];
      u16* out = &trans_keys[(static_cast<size_t>(f) * anim_trans.size() + i) * 3];
      const f32 comp[3] = {v.x, v.y, v.z};
      for (int c = 0; c < 3; ++c) {
        f32 step = rg[3 + c];
        f32 q = step > 0 ? (comp[c] - rg[c]) / step : 0.0f;
        out[c] = static_cast<u16>(std::clamp(q, 0.0f, 65535.0f) + 0.5f);
      }
    }
    for (size_t i = 0; i < anim_scale.size(); ++i) {
      f32 v = s_[static_cast<size_t>(f) * tracks_ + anim_scale[i]];
      f32 step = scale_range[i * 2 + 1];
      f32 q = step > 0 ? (v - scale_range[i * 2]) / step : 0.0f;
      scale_keys[static_cast<size_t>(f) * anim_scale.size() + i] =
          static_cast<u16>(std::clamp(q, 0.0f, 65535.0f) + 0.5f);
    }
  }

  // String block for event names.
  std::vector<char> strings;
  std::vector<EventRecord> events;
  for (const auto& [name, time] : events_) {
    events.push_back({HashName(name), time, static_cast<u32>(strings.size())});
    strings.insert(strings.end(), name.begin(), name.end());
    strings.push_back('\0');
  }
  std::sort(events.begin(), events.end(),
            [](const EventRecord& a, const EventRecord& b) { return a.time < b.time; });
  std::vector<RootKey> roots;
  for (const auto& [time, v] : root_keys_) roots.push_back({time, v.x, v.y, v.z});
  std::sort(roots.begin(), roots.end(),
            [](const RootKey& a, const RootKey& b) { return a.time < b.time; });

  // Assemble: header, then 8-aligned blocks.
  Header h{};
  h.magic = kMagic;
  h.version = kVersion;
  h.num_tracks = tracks_;
  h.num_frames = frames_;
  h.frame_rate = rate_;
  h.duration = frames_ > 1 ? static_cast<f32>(frames_ - 1) / rate_ : 0.0f;
  h.flags = additive_ ? kFlagAdditive : 0;
  h.num_anim_rot = static_cast<u32>(anim_rot.size());
  h.num_anim_trans = static_cast<u32>(anim_trans.size());
  h.num_anim_scale = static_cast<u32>(anim_scale.size());
  h.num_events = static_cast<u32>(events.size());
  h.num_root_keys = static_cast<u32>(roots.size());

  std::vector<u8> blob(sizeof(Header));
  auto append = [&blob](const void* data, size_t bytes) -> u64 {
    blob.resize((blob.size() + 7) & ~size_t{7});
    u64 at = blob.size();
    const u8* p = static_cast<const u8*>(data);
    blob.insert(blob.end(), p, p + bytes);
    return at;
  };
  // Constant pose: frame 0 of every channel (animated slots get overwritten
  // during sampling, so storing them too keeps the copy branch-free).
  std::vector<Vec3> const_t(t_.begin(), t_.begin() + tracks_);
  std::vector<Quat> const_r(rots.begin(), rots.begin() + tracks_);
  std::vector<f32> const_s(s_.begin(), s_.begin() + tracks_);
  h.off_const_t = append(const_t.data(), const_t.size() * sizeof(Vec3));
  h.off_const_r = append(const_r.data(), const_r.size() * sizeof(Quat));
  h.off_const_s = append(const_s.data(), const_s.size() * sizeof(f32));
  h.off_rot_idx = append(anim_rot.data(), anim_rot.size() * sizeof(u16));
  h.off_trans_idx = append(anim_trans.data(), anim_trans.size() * sizeof(u16));
  h.off_scale_idx = append(anim_scale.data(), anim_scale.size() * sizeof(u16));
  h.off_trans_range = append(trans_range.data(), trans_range.size() * sizeof(f32));
  h.off_scale_range = append(scale_range.data(), scale_range.size() * sizeof(f32));
  h.off_rot_keys = append(rot_keys.data(), rot_keys.size() * sizeof(u16));
  h.off_trans_keys = append(trans_keys.data(), trans_keys.size() * sizeof(u16));
  h.off_scale_keys = append(scale_keys.data(), scale_keys.size() * sizeof(u16));
  h.off_root_keys = append(roots.data(), roots.size() * sizeof(RootKey));
  h.off_events = append(events.data(), events.size() * sizeof(EventRecord));
  h.off_strings = append(strings.data(), strings.size());
  h.total_size = blob.size();
  std::memcpy(blob.data(), &h, sizeof(Header));
  return blob;
}

}  // namespace kinema

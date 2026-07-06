#ifndef KINEMA_KINEMA_H_
#define KINEMA_KINEMA_H_

// kinema: a small, data-oriented skeletal animation runtime.
//
// Design (the short version):
//  - Clips are transcoded once into a relocatable binary blob: uniformly
//    sampled keys, 16-bit range-quantized, stored frame-major SoA so sampling
//    a pose is two contiguous row reads and one lerp — no per-bone branching,
//    no curve evaluation, no allocation. Constant tracks are detected at build
//    time and stored once at full precision (zero error for static bones).
//  - Poses are structure-of-arrays views over caller/arena memory. Blend
//    operations are flat kernels over whole arrays.
//  - Blend trees execute as a compiled flat program (PoseOp list) over pose
//    registers, so the authoring structure never appears in the hot path.
//  - Transitions use inertialization (capture the pose delta at the switch,
//    decay it) so a transition evaluates ONE graph, not two.
//
// The library is self-contained (no engine dependencies, no exceptions, no
// RTTI); physics engines integrate through adapters (see jolt_adapter.h).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kinema {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;
using f32 = float;

struct Vec3 {
  f32 x = 0, y = 0, z = 0;
};
struct Quat {
  f32 x = 0, y = 0, z = 0, w = 1;
};

// FNV-1a: stable name identity for bones and events across projects.
constexpr u64 HashName(std::string_view name) {
  u64 h = 14695981039346656037ull;
  for (char c : name) {
    h ^= static_cast<u8>(c);
    h *= 1099511628211ull;
  }
  return h;
}

// ---------------------------------------------------------------------------
// Poses: SoA views. `count` bones; arrays are caller-owned (see PoseArena).

struct PoseView {
  Vec3* translation = nullptr;
  Quat* rotation = nullptr;
  f32* scale = nullptr;
  u32 count = 0;
};

struct ConstPoseView {
  const Vec3* translation = nullptr;
  const Quat* rotation = nullptr;
  const f32* scale = nullptr;
  u32 count = 0;
  ConstPoseView() = default;
  ConstPoseView(const PoseView& p)
      : translation(p.translation), rotation(p.rotation), scale(p.scale), count(p.count) {}
};

// Fixed-capacity pose scratch for one evaluation frame. Acquire() hands out
// registers; Reset() recycles them. No per-frame heap traffic.
class PoseArena {
 public:
  PoseArena() = default;
  PoseArena(u32 bones, u32 max_poses) { Init(bones, max_poses); }
  void Init(u32 bones, u32 max_poses);
  PoseView Acquire();          // aborts via assert in debug when exhausted
  PoseView At(u32 index);      // register access without advancing
  void Reset() { used_ = 0; }
  u32 bones() const { return bones_; }

 private:
  std::vector<Vec3> translations_;
  std::vector<Quat> rotations_;
  std::vector<f32> scales_;
  u32 bones_ = 0, capacity_ = 0, used_ = 0;
};

// ---------------------------------------------------------------------------
// Skeleton: parents + hashed names + bind pose. Model-space helper included;
// matrix palettes are the host engine's business.

struct Skeleton {
  std::vector<i16> parents;  // -1 = root
  std::vector<u64> name_hashes;
  std::vector<Vec3> bind_translation;
  std::vector<Quat> bind_rotation;
  std::vector<f32> bind_scale;

  u32 count() const { return static_cast<u32>(parents.size()); }
  // Index of the bone with this name hash, or -1.
  int Find(u64 name_hash) const;
};

// Accumulates local TRS down the hierarchy (parents must precede children).
void ComputeModelSpace(const Skeleton& skeleton, ConstPoseView local, Vec3* out_translation,
                       Quat* out_rotation, f32* out_scale);

// ---------------------------------------------------------------------------
// Compressed clips.

struct ClipEvent {
  u64 name_hash = 0;
  f32 time = 0;
  const char* name = nullptr;  // points into the clip blob
};

// Non-owning validated view over a clip blob (see ClipBuilder::Build for the
// layout). Blobs are relocatable and mmap-friendly: load bytes, call FromBlob.
class Clip {
 public:
  static std::optional<Clip> FromBlob(const u8* data, size_t size);

  u32 num_tracks() const;
  u32 num_frames() const;
  f32 frame_rate() const;
  f32 duration() const;
  bool additive() const;

  // Samples the local pose at `time` (clamped). out.count == num_tracks().
  void Sample(f32 time, PoseView out) const;

  // Cumulative root displacement at `time` (zero at t=0, held past the last
  // key). Delta between two times, wrapping through the end when t1 < t0.
  Vec3 RootTranslation(f32 time) const;
  Vec3 RootDelta(f32 t0, f32 t1) const;

  u32 num_events() const;
  ClipEvent Event(u32 index) const;
  // Invokes fn(const ClipEvent&) for events with time in (t0, t1]; when
  // t1 < t0 the range wraps through the clip end (looped playback).
  template <typename Fn>
  void EventsInRange(f32 t0, f32 t1, Fn&& fn) const {
    for (u32 i = 0; i < num_events(); ++i) {
      ClipEvent e = Event(i);
      bool hit = t1 >= t0 ? (e.time > t0 && e.time <= t1) : (e.time > t0 || e.time <= t1);
      if (hit) fn(e);
    }
  }

  const u8* blob() const { return blob_; }
  size_t blob_size() const;

 private:
  const u8* blob_ = nullptr;
};

// Clip that owns its blob bytes.
class OwnedClip {
 public:
  OwnedClip() = default;
  explicit OwnedClip(std::vector<u8> blob);
  const Clip* get() const { return clip_ ? &*clip_ : nullptr; }
  const Clip* operator->() const { return get(); }
  explicit operator bool() const { return clip_.has_value(); }
  const std::vector<u8>& bytes() const { return blob_; }

 private:
  std::vector<u8> blob_;
  std::optional<Clip> clip_;
};

// Feed uniformly sampled full poses (track-ordered), then Build() quantizes,
// splits constant from animated tracks and packs the blob.
class ClipBuilder {
 public:
  ClipBuilder(u32 num_tracks, u32 num_frames, f32 frame_rate);
  void SetSample(u32 frame, u32 track, const Vec3& t, const Quat& r, f32 s);
  void SetAdditive(bool additive) { additive_ = additive; }
  void AddEvent(std::string_view name, f32 time);
  // Sparse cumulative root-motion keys (time, displacement-from-start).
  void AddRootKey(f32 time, const Vec3& translation);
  std::vector<u8> Build() const;

 private:
  u32 tracks_, frames_;
  f32 rate_;
  bool additive_ = false;
  std::vector<Vec3> t_;  // [frame * tracks + track]
  std::vector<Quat> r_;
  std::vector<f32> s_;
  std::vector<std::pair<std::string, f32>> events_;
  std::vector<std::pair<f32, Vec3>> root_keys_;
};

// ---------------------------------------------------------------------------
// Blend kernels. All operate over whole SoA arrays; rotation blends resolve
// quaternion double-cover per bone (branchless sign select) and renormalize.

void CopyPose(ConstPoseView src, PoseView dst);
// dst = nlerp(a, b, alpha)
void BlendPoses(ConstPoseView a, ConstPoseView b, f32 alpha, PoseView dst);
// dst = nlerp(a, b, alpha * mask[bone]); mask may be null (uniform).
void BlendPosesMasked(ConstPoseView a, ConstPoseView b, f32 alpha, const f32* mask, PoseView dst);
// dst = base (+) add, weighted: rotations compose, translations add, scales
// multiply. `add` is an additive pose (delta from its reference).
void ApplyAdditive(ConstPoseView base, ConstPoseView add, f32 weight, PoseView dst);

// ---------------------------------------------------------------------------
// Compiled pose program: a flat op list over arena registers. Hosts compile
// their blend tree / state machine into this once per structural change and
// just patch times/alphas per frame.

struct PoseOp {
  enum class Kind : u8 { kSample, kCopy, kBlend, kBlendMasked, kAdditive };
  Kind kind = Kind::kCopy;
  u8 dst = 0, a = 0, b = 0;
  const Clip* clip = nullptr;  // kSample
  f32 time = 0;                // kSample
  f32 alpha = 0;               // kBlend*/kAdditive weight
  const f32* mask = nullptr;   // kBlendMasked, per-bone weights
};

// Executes ops in order against arena registers 0..N and returns the view of
// the last op's dst. The arena must hold max(dst,a,b)+1 registers.
PoseView ExecuteProgram(const PoseOp* ops, size_t count, PoseArena& arena);

// ---------------------------------------------------------------------------
// Inertialization: on a state switch, capture the offset between the pose the
// previous state would show and the new state's pose, then decay that offset
// smoothly while ONLY the new state is evaluated.

class Inertializer {
 public:
  void Init(u32 bones);
  // Capture offsets = from - to, start a blend of `duration` seconds.
  void Begin(ConstPoseView from, ConstPoseView to, f32 duration);
  // Applies the decayed offset onto pose; returns false once finished.
  bool Apply(PoseView pose, f32 dt);
  bool active() const { return remaining_ > 0; }

 private:
  std::vector<Vec3> dt_;
  std::vector<Vec3> dr_;  // rotation offsets, axis*angle
  std::vector<f32> ds_;
  f32 remaining_ = 0, duration_ = 0;
};

}  // namespace kinema

#endif  // KINEMA_KINEMA_H_

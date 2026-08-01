#include "world/cine_camera.h"

#include <cmath>

namespace rx::world {
namespace {

struct V3 {
  f32 x = 0, y = 0, z = 0;
};

V3 Load(const f32 v[3]) { return {v[0], v[1], v[2]}; }
void Store(const V3& v, f32 out[3]) {
  out[0] = v.x;
  out[1] = v.y;
  out[2] = v.z;
}
V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Scale(const V3& a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Mid(const V3& a, const V3& b) { return Scale(Add(a, b), 0.5f); }

// Planar direction from a to b; falls back to `yaw`'s forward when they coincide,
// so a lone speaker is still framed from in front rather than from inside.
V3 Axis(const V3& from, const V3& to, f32 yaw, f32 min_gap) {
  V3 d{to.x - from.x, 0, to.z - from.z};
  const f32 len = std::sqrt(d.x * d.x + d.z * d.z);
  if (len < min_gap) return {std::sin(yaw), 0, -std::cos(yaw)};
  return Scale(d, 1.0f / len);
}

V3 RightOf(const V3& axis) { return {-axis.z, 0, axis.x}; }

}  // namespace

CineFraming SolveShot(ShotKind kind, const f32 speaker_head[3], const f32 listener_head[3],
                      f32 speaker_yaw, const ShotParams& p) {
  const V3 speaker = Load(speaker_head);
  const V3 listener = Load(listener_head);
  // The conversation axis runs from the speaker to whoever they address, so
  // "toward the listener" is +axis and the shoulder offset is perpendicular to it.
  const V3 axis = Axis(speaker, listener, speaker_yaw, p.min_subject_gap);
  const V3 right = RightOf(axis);
  const V3 up{0, 1, 0};

  CineFraming out;
  switch (kind) {
    case ShotKind::kOverShoulder: {
      // Behind the listener's shoulder, looking back along the axis at the speaker.
      const V3 eye = Add(Add(Add(listener, Scale(axis, p.medium * 0.55f)),
                             Scale(right, p.shoulder)),
                         Scale(up, p.lift));
      Store(eye, out.eye);
      Store(speaker, out.target);
      break;
    }
    case ShotKind::kReverse: {
      // The mirrored angle: behind the speaker onto the listener, opposite shoulder.
      const V3 eye = Add(Add(Sub(speaker, Scale(axis, p.medium * 0.55f)),
                             Scale(right, -p.shoulder)),
                         Scale(up, p.lift));
      Store(eye, out.eye);
      Store(listener, out.target);
      break;
    }
    case ShotKind::kTwoShot: {
      const V3 mid = Mid(speaker, listener);
      const V3 eye = Add(Add(mid, Scale(right, p.wide * 0.62f)), Scale(up, p.lift * 2.0f));
      Store(eye, out.eye);
      Store(mid, out.target);
      break;
    }
    case ShotKind::kCloseUp: {
      const V3 eye = Add(Add(speaker, Scale(axis, p.close)), Scale(up, p.lift * 0.5f));
      Store(eye, out.eye);
      Store(speaker, out.target);
      break;
    }
    case ShotKind::kWide: {
      const V3 mid = Mid(speaker, listener);
      const V3 eye = Add(Add(Add(mid, Scale(right, p.wide)), Scale(axis, p.wide * 0.35f)),
                         Scale(up, p.wide * 0.28f));
      Store(eye, out.eye);
      Store(mid, out.target);
      break;
    }
  }
  return out;
}

ShotKind ShotDirector::PickKind() const {
  // Coverage pattern: the two reverse angles carry the exchange, every fourth cut
  // opens up to a two-shot and every sixth pushes in, which is enough variety for
  // a long scene without ever leaving the speaker out of frame.
  switch (cuts_ % 6) {
    case 0:
    case 2:
      return ShotKind::kOverShoulder;
    case 1:
      return ShotKind::kReverse;
    case 3:
      return ShotKind::kTwoShot;
    case 4:
      return ShotKind::kOverShoulder;
    default:
      return ShotKind::kCloseUp;
  }
}

bool ShotDirector::Update(f32 dt, u64 speaker, u64 addressee) {
  shot_time_ += dt;
  const bool subject_changed = speaker != speaker_ || addressee != addressee_;
  const bool held_too_long = max_shot_ > 0 && shot_time_ >= max_shot_;
  const bool may_cut = !started_ || shot_time_ >= min_shot_;
  if (!started_ || (may_cut && (subject_changed || held_too_long))) {
    if (started_) ++cuts_;
    started_ = true;
    speaker_ = speaker;
    addressee_ = addressee;
    kind_ = PickKind();
    shot_time_ = 0;
    return true;
  }
  // A change inside the minimum hold is not dropped: speaker_/addressee_ stay the
  // subjects of the shot on screen, so the cut lands on the first eligible frame.
  return false;
}

void ShotDirector::Reset() {
  started_ = false;
  speaker_ = 0;
  addressee_ = 0;
  shot_time_ = 0;
  cuts_ = 0;
  kind_ = ShotKind::kOverShoulder;
}

CineFraming EaseFraming(const CineFraming& from, const CineFraming& to, f32 dt, f32 rate) {
  const f32 t = rate <= 0 ? 1.0f : 1.0f - std::exp(-rate * dt);
  CineFraming out;
  for (int i = 0; i < 3; ++i) {
    out.eye[i] = from.eye[i] + (to.eye[i] - from.eye[i]) * t;
    out.target[i] = from.target[i] + (to.target[i] - from.target[i]) * t;
  }
  return out;
}

CineFraming PushIn(const CineFraming& shot, f32 held, f32 rate, f32 limit) {
  const f32 amount = std::min(std::max(held, 0.0f) * rate, limit);
  CineFraming out = shot;
  for (int axis = 0; axis < 3; ++axis)
    out.eye[axis] = shot.target[axis] + (shot.eye[axis] - shot.target[axis]) * (1.0f - amount);
  return out;
}

}  // namespace rx::world

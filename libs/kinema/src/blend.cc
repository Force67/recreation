#include <cassert>
#include <cmath>
#include <cstring>

#include "kinema/kinema.h"

namespace kinema {
namespace {

inline Quat Normalize(f32 x, f32 y, f32 z, f32 w) {
  f32 len2 = x * x + y * y + z * z + w * w;
  f32 inv = len2 > 1e-12f ? 1.0f / std::sqrt(len2) : 0.0f;
  return Quat{x * inv, y * inv, z * inv, w * inv};
}

inline Quat Mul(const Quat& a, const Quat& b) {
  return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// nlerp with double-cover resolved by a branchless sign flip on b.
inline Quat Nlerp(const Quat& a, const Quat& b, f32 t) {
  f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  f32 sign = dot >= 0 ? 1.0f : -1.0f;
  return Normalize(a.x + (b.x * sign - a.x) * t, a.y + (b.y * sign - a.y) * t,
                   a.z + (b.z * sign - a.z) * t, a.w + (b.w * sign - a.w) * t);
}

// log/exp of unit quaternions as axis*angle vectors (for offsets).
inline Vec3 Log(const Quat& q) {
  f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (len < 1e-8f) return Vec3{};
  f32 angle = 2.0f * std::atan2(len, q.w);
  f32 s = angle / len;
  return Vec3{q.x * s, q.y * s, q.z * s};
}

inline Quat Exp(const Vec3& v) {
  f32 angle = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (angle < 1e-8f) return Quat{};
  f32 half = angle * 0.5f;
  f32 s = std::sin(half) / angle;
  return Quat{v.x * s, v.y * s, v.z * s, std::cos(half)};
}

inline Quat Conjugate(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

}  // namespace

// ---------------------------------------------------------------------------
// Arena / skeleton

void PoseArena::Init(u32 bones, u32 max_poses) {
  bones_ = bones;
  capacity_ = max_poses;
  used_ = 0;
  translations_.resize(static_cast<size_t>(bones) * max_poses);
  rotations_.resize(static_cast<size_t>(bones) * max_poses);
  scales_.resize(static_cast<size_t>(bones) * max_poses);
}

PoseView PoseArena::Acquire() {
  assert(used_ < capacity_);
  return At(used_++);
}

PoseView PoseArena::At(u32 index) {
  assert(index < capacity_);
  size_t base = static_cast<size_t>(index) * bones_;
  return PoseView{translations_.data() + base, rotations_.data() + base, scales_.data() + base,
                  bones_};
}

int Skeleton::Find(u64 name_hash) const {
  for (size_t i = 0; i < name_hashes.size(); ++i) {
    if (name_hashes[i] == name_hash) return static_cast<int>(i);
  }
  return -1;
}

void ComputeModelSpace(const Skeleton& skeleton, ConstPoseView local, Vec3* out_t, Quat* out_r,
                       f32* out_s) {
  const u32 n = skeleton.count();
  assert(local.count == n);
  for (u32 i = 0; i < n; ++i) {
    int p = skeleton.parents[i];
    if (p < 0) {
      out_t[i] = local.translation[i];
      out_r[i] = local.rotation[i];
      out_s[i] = local.scale[i];
      continue;
    }
    const Quat& pr = out_r[p];
    Vec3 v{local.translation[i].x * out_s[p], local.translation[i].y * out_s[p],
           local.translation[i].z * out_s[p]};
    // v' = pr * v * pr^-1
    Quat t = Mul(pr, Quat{v.x, v.y, v.z, 0});
    Quat rotated = Mul(t, Conjugate(pr));
    out_t[i] = Vec3{out_t[p].x + rotated.x, out_t[p].y + rotated.y, out_t[p].z + rotated.z};
    Quat r = Mul(pr, local.rotation[i]);
    out_r[i] = Normalize(r.x, r.y, r.z, r.w);
    out_s[i] = out_s[p] * local.scale[i];
  }
}

// ---------------------------------------------------------------------------
// Blend kernels

void CopyPose(ConstPoseView src, PoseView dst) {
  assert(src.count == dst.count);
  std::memcpy(dst.translation, src.translation, src.count * sizeof(Vec3));
  std::memcpy(dst.rotation, src.rotation, src.count * sizeof(Quat));
  std::memcpy(dst.scale, src.scale, src.count * sizeof(f32));
}

void BlendPoses(ConstPoseView a, ConstPoseView b, f32 alpha, PoseView dst) {
  BlendPosesMasked(a, b, alpha, nullptr, dst);
}

void BlendPosesMasked(ConstPoseView a, ConstPoseView b, f32 alpha, const f32* mask,
                      PoseView dst) {
  assert(a.count == b.count && a.count == dst.count);
  const u32 n = a.count;
  for (u32 i = 0; i < n; ++i) {
    f32 t = mask ? alpha * mask[i] : alpha;
    dst.translation[i] =
        Vec3{a.translation[i].x + (b.translation[i].x - a.translation[i].x) * t,
             a.translation[i].y + (b.translation[i].y - a.translation[i].y) * t,
             a.translation[i].z + (b.translation[i].z - a.translation[i].z) * t};
    dst.rotation[i] = Nlerp(a.rotation[i], b.rotation[i], t);
    dst.scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * t;
  }
}

void ApplyAdditive(ConstPoseView base, ConstPoseView add, f32 weight, PoseView dst) {
  assert(base.count == add.count && base.count == dst.count);
  const u32 n = base.count;
  const Quat identity{};
  for (u32 i = 0; i < n; ++i) {
    // Weighted delta: nlerp the additive parts toward identity/zero.
    Quat dq = Nlerp(identity, add.rotation[i], weight);
    Quat q = Mul(base.rotation[i], dq);
    dst.rotation[i] = Normalize(q.x, q.y, q.z, q.w);
    dst.translation[i] = Vec3{base.translation[i].x + add.translation[i].x * weight,
                              base.translation[i].y + add.translation[i].y * weight,
                              base.translation[i].z + add.translation[i].z * weight};
    dst.scale[i] = base.scale[i] * (1.0f + (add.scale[i] - 1.0f) * weight);
  }
}

// ---------------------------------------------------------------------------
// Program

PoseView ExecuteProgram(const PoseOp* ops, size_t count, PoseArena& arena) {
  PoseView last{};
  for (size_t i = 0; i < count; ++i) {
    const PoseOp& op = ops[i];
    PoseView dst = arena.At(op.dst);
    switch (op.kind) {
      case PoseOp::Kind::kSample:
        op.clip->Sample(op.time, dst);
        break;
      case PoseOp::Kind::kCopy:
        CopyPose(arena.At(op.a), dst);
        break;
      case PoseOp::Kind::kBlend:
        BlendPoses(arena.At(op.a), arena.At(op.b), op.alpha, dst);
        break;
      case PoseOp::Kind::kBlendMasked:
        BlendPosesMasked(arena.At(op.a), arena.At(op.b), op.alpha, op.mask, dst);
        break;
      case PoseOp::Kind::kAdditive:
        ApplyAdditive(arena.At(op.a), arena.At(op.b), op.alpha, dst);
        break;
    }
    last = dst;
  }
  return last;
}

// ---------------------------------------------------------------------------
// Inertialization

void Inertializer::Init(u32 bones) {
  dt_.assign(bones, Vec3{});
  dr_.assign(bones, Vec3{});
  ds_.assign(bones, 0.0f);
  remaining_ = duration_ = 0;
}

void Inertializer::Begin(ConstPoseView from, ConstPoseView to, f32 duration) {
  assert(from.count == to.count && from.count == dt_.size());
  const u32 n = from.count;
  for (u32 i = 0; i < n; ++i) {
    dt_[i] = Vec3{from.translation[i].x - to.translation[i].x,
                  from.translation[i].y - to.translation[i].y,
                  from.translation[i].z - to.translation[i].z};
    // Offset rotation carrying `to` onto `from`, shortest arc.
    Quat rel = Mul(from.rotation[i], Conjugate(to.rotation[i]));
    if (rel.w < 0) rel = Quat{-rel.x, -rel.y, -rel.z, -rel.w};
    dr_[i] = Log(rel);
    ds_[i] = from.scale[i] - to.scale[i];
  }
  duration_ = remaining_ = std::max(duration, 1e-4f);
}

bool Inertializer::Apply(PoseView pose, f32 dt) {
  if (remaining_ <= 0) return false;
  remaining_ = std::max(remaining_ - dt, 0.0f);
  // Smootherstep complement: C2 at both ends, so the offset fades without a
  // velocity pop at the switch or at the finish.
  f32 x = 1.0f - remaining_ / duration_;
  f32 h = 1.0f - x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
  if (h <= 0) return false;
  const u32 n = pose.count;
  for (u32 i = 0; i < n; ++i) {
    pose.translation[i].x += dt_[i].x * h;
    pose.translation[i].y += dt_[i].y * h;
    pose.translation[i].z += dt_[i].z * h;
    Quat off = Exp(Vec3{dr_[i].x * h, dr_[i].y * h, dr_[i].z * h});
    Quat q = Mul(off, pose.rotation[i]);
    pose.rotation[i] = Normalize(q.x, q.y, q.z, q.w);
    pose.scale[i] += ds_[i] * h;
  }
  return remaining_ > 0;
}

}  // namespace kinema

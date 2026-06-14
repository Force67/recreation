#ifndef RECREATION_CORE_MATH_H_
#define RECREATION_CORE_MATH_H_

#include <cmath>

#include "core/types.h"

namespace rec {

struct Vec3 {
  f32 x = 0;
  f32 y = 0;
  f32 z = 0;
};

inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

inline Vec3 operator*(const Vec3& v, f32 s) { return {v.x * s, v.y * s, v.z * s}; }

inline Vec3& operator+=(Vec3& a, const Vec3& b) {
  a = a + b;
  return a;
}

inline f32 Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 Normalize(const Vec3& v) {
  f32 length = std::sqrt(Dot(v, v));
  return length > 0 ? Vec3{v.x / length, v.y / length, v.z / length} : v;
}

inline f32 Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

inline Vec3 Lerp(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

// Quaternion (x, y, z, w), matching the engine Transform layout. Used for bone
// local rotations: blendable, then converted to a matrix for skinning.
struct Quat {
  f32 x = 0, y = 0, z = 0, w = 1;
};

inline Quat operator*(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat Normalize(const Quat& q) {
  f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 0) return {0, 0, 0, 1};
  f32 inv = 1.0f / len;
  return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

inline Quat QuatFromAxisAngle(const Vec3& axis, f32 radians) {
  Vec3 n = Normalize(axis);
  f32 s = std::sin(radians * 0.5f);
  return {n.x * s, n.y * s, n.z * s, std::cos(radians * 0.5f)};
}

inline Quat Conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

// Rotate a vector by a unit quaternion: v + 2 s (u x v) + 2 (u x (u x v)).
inline Vec3 Rotate(const Quat& q, const Vec3& v) {
  Vec3 u{q.x, q.y, q.z};
  Vec3 t = Cross(u, v) * 2.0f;
  return v + t * q.w + Cross(u, t);
}

// Shortest-arc rotation taking `from` onto `to`.
inline Quat QuatBetween(const Vec3& from, const Vec3& to) {
  Vec3 f = Normalize(from);
  Vec3 t = Normalize(to);
  f32 d = Dot(f, t);
  if (d >= 1.0f - 1e-6f) return {0, 0, 0, 1};
  if (d <= -1.0f + 1e-6f) {
    Vec3 axis = Cross({1, 0, 0}, f);
    if (Length(axis) < 1e-4f) axis = Cross({0, 1, 0}, f);
    return QuatFromAxisAngle(axis, 3.14159265358979f);
  }
  Vec3 axis = Cross(f, t);
  return Normalize(Quat{axis.x, axis.y, axis.z, 1.0f + d});
}

// Shortest-arc interpolation. Falls back to nlerp for nearly parallel inputs.
inline Quat Slerp(const Quat& a, Quat b, f32 t) {
  f32 cos_theta = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (cos_theta < 0) {
    b = {-b.x, -b.y, -b.z, -b.w};
    cos_theta = -cos_theta;
  }
  if (cos_theta > 0.9995f) {
    return Normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                          a.w + (b.w - a.w) * t});
  }
  f32 theta = std::acos(cos_theta);
  f32 sin_theta = std::sin(theta);
  f32 wa = std::sin((1 - t) * theta) / sin_theta;
  f32 wb = std::sin(t * theta) / sin_theta;
  return {wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
}

// Column major, m[column * 4 + row]. Matches std140/std430 mat4 layout so
// the struct can go straight into push constants.
struct Mat4 {
  f32 m[16] = {};

  static Mat4 Identity() {
    Mat4 result;
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    return result;
  }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 result;
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      f32 sum = 0;
      for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
      result.m[col * 4 + row] = sum;
    }
  }
  return result;
}

inline Mat4 MakeTranslation(const Vec3& v) {
  Mat4 result = Mat4::Identity();
  result.m[12] = v.x;
  result.m[13] = v.y;
  result.m[14] = v.z;
  return result;
}

inline Mat4 MakeScale(f32 s) {
  Mat4 result = Mat4::Identity();
  result.m[0] = result.m[5] = result.m[10] = s;
  return result;
}

inline Mat4 MakeFromQuat(f32 x, f32 y, f32 z, f32 w) {
  Mat4 r = Mat4::Identity();
  r.m[0] = 1 - 2 * (y * y + z * z);
  r.m[1] = 2 * (x * y + w * z);
  r.m[2] = 2 * (x * z - w * y);
  r.m[4] = 2 * (x * y - w * z);
  r.m[5] = 1 - 2 * (x * x + z * z);
  r.m[6] = 2 * (y * z + w * x);
  r.m[8] = 2 * (x * z + w * y);
  r.m[9] = 2 * (y * z - w * x);
  r.m[10] = 1 - 2 * (x * x + y * y);
  return r;
}

inline Mat4 MakeFromQuat(const Quat& q) { return MakeFromQuat(q.x, q.y, q.z, q.w); }

// Affine bone/node transform: translate * rotate * uniform scale, column major.
inline Mat4 MakeTransform(const Vec3& translation, const Quat& rotation, f32 scale) {
  Mat4 m = MakeFromQuat(rotation);
  for (int i = 0; i < 12; ++i) m.m[i] *= scale;  // scale the 3x3 columns
  m.m[12] = translation.x;
  m.m[13] = translation.y;
  m.m[14] = translation.z;
  return m;
}

// General 4x4 inverse via the adjugate. Fine for per-frame camera matrices.
inline Mat4 Inverse(const Mat4& m) {
  const f32* a = m.m;
  f32 b00 = a[0] * a[5] - a[1] * a[4];
  f32 b01 = a[0] * a[6] - a[2] * a[4];
  f32 b02 = a[0] * a[7] - a[3] * a[4];
  f32 b03 = a[1] * a[6] - a[2] * a[5];
  f32 b04 = a[1] * a[7] - a[3] * a[5];
  f32 b05 = a[2] * a[7] - a[3] * a[6];
  f32 b06 = a[8] * a[13] - a[9] * a[12];
  f32 b07 = a[8] * a[14] - a[10] * a[12];
  f32 b08 = a[8] * a[15] - a[11] * a[12];
  f32 b09 = a[9] * a[14] - a[10] * a[13];
  f32 b10 = a[9] * a[15] - a[11] * a[13];
  f32 b11 = a[10] * a[15] - a[11] * a[14];

  f32 det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (det == 0) return Mat4::Identity();
  f32 inv = 1.0f / det;

  Mat4 r;
  r.m[0] = (a[5] * b11 - a[6] * b10 + a[7] * b09) * inv;
  r.m[1] = (a[2] * b10 - a[1] * b11 - a[3] * b09) * inv;
  r.m[2] = (a[13] * b05 - a[14] * b04 + a[15] * b03) * inv;
  r.m[3] = (a[10] * b04 - a[9] * b05 - a[11] * b03) * inv;
  r.m[4] = (a[6] * b08 - a[4] * b11 - a[7] * b07) * inv;
  r.m[5] = (a[0] * b11 - a[2] * b08 + a[3] * b07) * inv;
  r.m[6] = (a[14] * b02 - a[12] * b05 - a[15] * b01) * inv;
  r.m[7] = (a[8] * b05 - a[10] * b02 + a[11] * b01) * inv;
  r.m[8] = (a[4] * b10 - a[5] * b08 + a[7] * b06) * inv;
  r.m[9] = (a[1] * b08 - a[0] * b10 - a[3] * b06) * inv;
  r.m[10] = (a[12] * b04 - a[13] * b02 + a[15] * b00) * inv;
  r.m[11] = (a[9] * b02 - a[8] * b04 - a[11] * b00) * inv;
  r.m[12] = (a[5] * b07 - a[4] * b09 - a[6] * b06) * inv;
  r.m[13] = (a[0] * b09 - a[1] * b07 + a[2] * b06) * inv;
  r.m[14] = (a[13] * b01 - a[12] * b03 - a[14] * b00) * inv;
  r.m[15] = (a[8] * b03 - a[9] * b01 + a[10] * b00) * inv;
  return r;
}

inline Vec3 Translation(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }

inline Vec3 TransformPoint(const Mat4& m, const Vec3& p) {
  return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
          m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
          m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

inline Vec3 TransformDir(const Mat4& m, const Vec3& d) {
  return {m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
          m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
          m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z};
}

// Rotation of the upper-left 3x3 of a column-major matrix, columns normalized
// to drop uniform scale. m[col*4+row] == R[row][col].
inline Quat QuatFromMat4(const Mat4& m) {
  Vec3 c0 = Normalize(Vec3{m.m[0], m.m[1], m.m[2]});
  Vec3 c1 = Normalize(Vec3{m.m[4], m.m[5], m.m[6]});
  Vec3 c2 = Normalize(Vec3{m.m[8], m.m[9], m.m[10]});
  f32 r00 = c0.x, r10 = c0.y, r20 = c0.z;
  f32 r01 = c1.x, r11 = c1.y, r21 = c1.z;
  f32 r02 = c2.x, r12 = c2.y, r22 = c2.z;
  f32 trace = r00 + r11 + r22;
  Quat q;
  if (trace > 0) {
    f32 s = 0.5f / std::sqrt(trace + 1.0f);
    q.w = 0.25f / s;
    q.x = (r21 - r12) * s;
    q.y = (r02 - r20) * s;
    q.z = (r10 - r01) * s;
  } else if (r00 > r11 && r00 > r22) {
    f32 s = 2.0f * std::sqrt(1.0f + r00 - r11 - r22);
    q.w = (r21 - r12) / s;
    q.x = 0.25f * s;
    q.y = (r01 + r10) / s;
    q.z = (r02 + r20) / s;
  } else if (r11 > r22) {
    f32 s = 2.0f * std::sqrt(1.0f + r11 - r00 - r22);
    q.w = (r02 - r20) / s;
    q.x = (r01 + r10) / s;
    q.y = 0.25f * s;
    q.z = (r12 + r21) / s;
  } else {
    f32 s = 2.0f * std::sqrt(1.0f + r22 - r00 - r11);
    q.w = (r10 - r01) / s;
    q.x = (r02 + r20) / s;
    q.y = (r12 + r21) / s;
    q.z = 0.25f * s;
  }
  return Normalize(q);
}

// Right handed, camera looks down -z in view space.
inline Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
  Vec3 f = Normalize(target - eye);
  Vec3 s = Normalize(Cross(f, up));
  Vec3 u = Cross(s, f);
  Mat4 r = Mat4::Identity();
  r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
  r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
  r.m[12] = -Dot(s, eye);
  r.m[13] = -Dot(u, eye);
  r.m[14] = Dot(f, eye);
  return r;
}

// Reversed z with an infinite far plane: near maps to depth 1, infinity to
// 0. Paired with GREATER depth tests this keeps precision uniform across the
// whole range, which matters for bethesda sized exteriors. Y is flipped for
// vulkan clip space.
inline Mat4 PerspectiveReversedZ(f32 fov_y_radians, f32 aspect, f32 near_plane) {
  f32 g = 1.0f / std::tan(fov_y_radians * 0.5f);
  Mat4 r;
  r.m[0] = g / aspect;
  r.m[5] = -g;
  r.m[11] = -1.0f;
  r.m[14] = near_plane;
  return r;
}

// Right-handed orthographic projection with a [0,1] depth range (Vulkan), no y
// flip. Used for shadow cascades: the map is rendered and sampled with the same
// matrix, so it stays self-consistent without matching the camera's clip space.
inline Mat4 Orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_plane, f32 far_plane) {
  Mat4 r;
  r.m[0] = 2.0f / (right - left);
  r.m[5] = 2.0f / (top - bottom);
  r.m[10] = -1.0f / (far_plane - near_plane);
  r.m[12] = -(right + left) / (right - left);
  r.m[13] = -(top + bottom) / (top - bottom);
  r.m[14] = -near_plane / (far_plane - near_plane);
  r.m[15] = 1.0f;
  return r;
}

}  // namespace rec

#endif  // RECREATION_CORE_MATH_H_

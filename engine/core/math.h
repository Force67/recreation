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

}  // namespace rec

#endif  // RECREATION_CORE_MATH_H_

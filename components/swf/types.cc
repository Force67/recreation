#include "components/swf/types.h"

#include <base/algorithm.h>

namespace rx::swf {
namespace {

u8 ClampChannel(f32 v) {
  if (v <= 0)
    return 0;
  if (v >= 255)
    return 255;
  return static_cast<u8>(v + 0.5f);
}

}  // namespace

Matrix Concat(const Matrix& parent, const Matrix& child) {
  Matrix out;
  out.scale_x = parent.scale_x * child.scale_x + parent.rotate_skew1 * child.rotate_skew0;
  out.rotate_skew0 =
      parent.rotate_skew0 * child.scale_x + parent.scale_y * child.rotate_skew0;
  out.rotate_skew1 =
      parent.scale_x * child.rotate_skew1 + parent.rotate_skew1 * child.scale_y;
  out.scale_y = parent.rotate_skew0 * child.rotate_skew1 + parent.scale_y * child.scale_y;
  const f32 tx = static_cast<f32>(child.translate_x);
  const f32 ty = static_cast<f32>(child.translate_y);
  out.translate_x = parent.translate_x +
                    static_cast<i32>(parent.scale_x * tx + parent.rotate_skew1 * ty);
  out.translate_y = parent.translate_y +
                    static_cast<i32>(parent.rotate_skew0 * tx + parent.scale_y * ty);
  return out;
}

Rect Transform(const Matrix& m, const Rect& r) {
  // Map all four corners: a rotated or skewed matrix turns the axis-aligned
  // input into a quad, and the caller wants its bounding box back.
  const f32 xs[4] = {static_cast<f32>(r.x_min), static_cast<f32>(r.x_max),
                     static_cast<f32>(r.x_min), static_cast<f32>(r.x_max)};
  const f32 ys[4] = {static_cast<f32>(r.y_min), static_cast<f32>(r.y_min),
                     static_cast<f32>(r.y_max), static_cast<f32>(r.y_max)};
  Rect out;
  for (int i = 0; i < 4; ++i) {
    const f32 x = m.scale_x * xs[i] + m.rotate_skew1 * ys[i] +
                  static_cast<f32>(m.translate_x);
    const f32 y = m.rotate_skew0 * xs[i] + m.scale_y * ys[i] +
                  static_cast<f32>(m.translate_y);
    const i32 xi = static_cast<i32>(x);
    const i32 yi = static_cast<i32>(y);
    if (i == 0) {
      out.x_min = out.x_max = xi;
      out.y_min = out.y_max = yi;
    } else {
      out.x_min = base::Min(out.x_min, xi);
      out.x_max = base::Max(out.x_max, xi);
      out.y_min = base::Min(out.y_min, yi);
      out.y_max = base::Max(out.y_max, yi);
    }
  }
  return out;
}

Rgba Apply(const ColorTransform& cx, Rgba color) {
  Rgba out;
  out.r = ClampChannel(static_cast<f32>(color.r) * cx.mul_r + static_cast<f32>(cx.add_r));
  out.g = ClampChannel(static_cast<f32>(color.g) * cx.mul_g + static_cast<f32>(cx.add_g));
  out.b = ClampChannel(static_cast<f32>(color.b) * cx.mul_b + static_cast<f32>(cx.add_b));
  out.a = ClampChannel(static_cast<f32>(color.a) * cx.mul_a + static_cast<f32>(cx.add_a));
  return out;
}

ColorTransform Concat(const ColorTransform& parent, const ColorTransform& child) {
  ColorTransform out;
  out.mul_r = parent.mul_r * child.mul_r;
  out.mul_g = parent.mul_g * child.mul_g;
  out.mul_b = parent.mul_b * child.mul_b;
  out.mul_a = parent.mul_a * child.mul_a;
  out.add_r = static_cast<i16>(parent.add_r + static_cast<i16>(parent.mul_r * child.add_r));
  out.add_g = static_cast<i16>(parent.add_g + static_cast<i16>(parent.mul_g * child.add_g));
  out.add_b = static_cast<i16>(parent.add_b + static_cast<i16>(parent.mul_b * child.add_b));
  out.add_a = static_cast<i16>(parent.add_a + static_cast<i16>(parent.mul_a * child.add_a));
  return out;
}

Rect Reader::ReadRect() {
  Align();
  const u32 bits = Bits(5);
  Rect r;
  r.x_min = SignedBits(bits);
  r.x_max = SignedBits(bits);
  r.y_min = SignedBits(bits);
  r.y_max = SignedBits(bits);
  Align();
  return r;
}

Matrix Reader::ReadMatrix() {
  Align();
  Matrix m;
  if (Bits(1)) {
    const u32 bits = Bits(5);
    m.scale_x = FixedBits(bits);
    m.scale_y = FixedBits(bits);
  }
  if (Bits(1)) {
    const u32 bits = Bits(5);
    m.rotate_skew0 = FixedBits(bits);
    m.rotate_skew1 = FixedBits(bits);
  }
  const u32 translate_bits = Bits(5);
  m.translate_x = SignedBits(translate_bits);
  m.translate_y = SignedBits(translate_bits);
  Align();
  return m;
}

ColorTransform Reader::ReadColorTransform(bool with_alpha) {
  Align();
  ColorTransform cx;
  const u32 has_add = Bits(1);
  const u32 has_mul = Bits(1);
  const u32 bits = Bits(4);
  if (has_mul) {
    cx.mul_r = static_cast<f32>(SignedBits(bits)) / 256.0f;
    cx.mul_g = static_cast<f32>(SignedBits(bits)) / 256.0f;
    cx.mul_b = static_cast<f32>(SignedBits(bits)) / 256.0f;
    if (with_alpha)
      cx.mul_a = static_cast<f32>(SignedBits(bits)) / 256.0f;
  }
  if (has_add) {
    cx.add_r = static_cast<i16>(SignedBits(bits));
    cx.add_g = static_cast<i16>(SignedBits(bits));
    cx.add_b = static_cast<i16>(SignedBits(bits));
    if (with_alpha)
      cx.add_a = static_cast<i16>(SignedBits(bits));
  }
  Align();
  return cx;
}

Rgba Reader::ReadRgb() {
  Rgba c;
  c.r = U8();
  c.g = U8();
  c.b = U8();
  c.a = 255;
  return c;
}

Rgba Reader::ReadRgba() {
  Rgba c;
  c.r = U8();
  c.g = U8();
  c.b = U8();
  c.a = U8();
  return c;
}

}  // namespace rx::swf

#ifndef RECREATION_SWF_TYPES_H_
#define RECREATION_SWF_TYPES_H_

#include <base/strings/xstring.h>

#include <cmath>
#include <cstring>

#include "core/types.h"

namespace rx::swf {

// SWF geometry is in twips, 1/20 of a pixel. Everything the readers hand back
// stays in twips; only the exporters divide down to pixels.
constexpr f32 kTwipsPerPixel = 20.0f;

constexpr f32 ToPixels(i32 twips) {
  return static_cast<f32>(twips) / kTwipsPerPixel;
}

struct Rect {
  i32 x_min = 0;
  i32 x_max = 0;
  i32 y_min = 0;
  i32 y_max = 0;

  i32 width() const { return x_max - x_min; }
  i32 height() const { return y_max - y_min; }
  bool empty() const { return x_max <= x_min || y_max <= y_min; }
};

// [scale_x rotate_skew1 translate_x]
// [rotate_skew0 scale_y translate_y]
// Translation is in twips, the four multipliers are plain floats.
struct Matrix {
  f32 scale_x = 1;
  f32 scale_y = 1;
  f32 rotate_skew0 = 0;  // row 1 column 0
  f32 rotate_skew1 = 0;  // row 0 column 1
  i32 translate_x = 0;
  i32 translate_y = 0;

  bool IsIdentity() const {
    return scale_x == 1 && scale_y == 1 && rotate_skew0 == 0 && rotate_skew1 == 0 &&
           translate_x == 0 && translate_y == 0;
  }

  // Degrees, positive clockwise (SWF's y axis points down).
  f32 RotationDegrees() const {
    return std::atan2(rotate_skew0, scale_x) * 57.29577951308232f;
  }
};

// child placed inside parent: parent * child.
Matrix Concat(const Matrix& parent, const Matrix& child);
Rect Transform(const Matrix& m, const Rect& r);

struct Rgba {
  u8 r = 0;
  u8 g = 0;
  u8 b = 0;
  u8 a = 255;

  bool operator==(const Rgba& o) const {
    return r == o.r && g == o.g && b == o.b && a == o.a;
  }
};

// Multiply-then-add, per channel. Values are the SWF fixed-point converted to
// float multipliers and integral addends.
struct ColorTransform {
  f32 mul_r = 1, mul_g = 1, mul_b = 1, mul_a = 1;
  i16 add_r = 0, add_g = 0, add_b = 0, add_a = 0;

  bool IsIdentity() const {
    return mul_r == 1 && mul_g == 1 && mul_b == 1 && mul_a == 1 && add_r == 0 &&
           add_g == 0 && add_b == 0 && add_a == 0;
  }
};

Rgba Apply(const ColorTransform& cx, Rgba color);
ColorTransform Concat(const ColorTransform& parent, const ColorTransform& child);

// Bit-and-byte cursor over a tag body. Every accessor bounds-checks and latches
// `ok` false on the first overrun, so a truncated tag yields zeros rather than
// reading past the buffer; callers check ok() once at the end.
class Reader {
 public:
  explicit Reader(ByteSpan data) : data_(data) {}

  bool ok() const { return ok_; }
  mem_size pos() const { return pos_; }
  mem_size size() const { return data_.size(); }
  bool eof() const { return pos_ >= data_.size(); }
  mem_size remaining() const { return pos_ < data_.size() ? data_.size() - pos_ : 0; }

  void Seek(mem_size p) {
    Align();
    if (p > data_.size())
      ok_ = false;
    else
      pos_ = p;
  }

  void Skip(mem_size n) {
    Align();
    if (pos_ + n > data_.size())
      ok_ = false;
    else
      pos_ += n;
  }

  u8 U8() {
    Align();
    if (pos_ + 1 > data_.size()) {
      ok_ = false;
      return 0;
    }
    return data_[pos_++];
  }

  u16 U16() {
    u16 lo = U8();
    return static_cast<u16>(lo | (static_cast<u16>(U8()) << 8));
  }

  u32 U32() {
    u32 lo = U16();
    return lo | (static_cast<u32>(U16()) << 16);
  }

  i8 I8() { return static_cast<i8>(U8()); }
  i16 I16() { return static_cast<i16>(U16()); }
  i32 I32() { return static_cast<i32>(U32()); }

  f32 Fixed8() { return static_cast<f32>(I16()) / 256.0f; }
  f32 Fixed16() { return static_cast<f32>(I32()) / 65536.0f; }

  f32 F32() {
    u32 bits = U32();
    f32 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
  }

  f64 F64() {
    u64 lo = U32();
    u64 hi = U32();
    u64 bits = lo | (hi << 32);
    f64 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
  }

  // AVM1 pushes doubles as two 32-bit halves in the opposite order to F64.
  f64 F64Swapped() {
    u64 hi = U32();
    u64 lo = U32();
    u64 bits = lo | (hi << 32);
    f64 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
  }

  // Null-terminated, latin-1/utf-8 as authored.
  base::String Str() {
    Align();
    base::String out;
    while (pos_ < data_.size()) {
      char c = static_cast<char>(data_[pos_++]);
      if (c == '\0')
        return out;
      out.push_back(c);
    }
    ok_ = false;
    return out;
  }

  base::String StrN(mem_size n) {
    Align();
    base::String out;
    if (pos_ + n > data_.size()) {
      ok_ = false;
      return out;
    }
    for (mem_size i = 0; i < n; ++i)
      out.push_back(static_cast<char>(data_[pos_ + i]));
    pos_ += n;
    return out;
  }

  ByteSpan Bytes(mem_size n) {
    Align();
    if (pos_ + n > data_.size()) {
      ok_ = false;
      return {};
    }
    ByteSpan out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
  }

  ByteSpan Rest() {
    Align();
    if (pos_ > data_.size())
      return {};
    ByteSpan out = data_.subspan(pos_);
    pos_ = data_.size();
    return out;
  }

  // Moves to the next byte boundary. A bit field that ends mid-byte leaves the
  // rest of that byte as padding, so the byte itself has to be stepped over.
  void Align() {
    if (bit_ != 0) {
      bit_ = 0;
      ++pos_;
    }
  }

  // Big-endian bit packing, MSB first, as every SWF bit field uses.
  u32 Bits(u32 count) {
    u32 out = 0;
    for (u32 i = 0; i < count; ++i) {
      if (pos_ >= data_.size()) {
        ok_ = false;
        return out;
      }
      const u32 bit = (data_[pos_] >> (7 - bit_)) & 1u;
      out = (out << 1) | bit;
      if (++bit_ == 8) {
        bit_ = 0;
        ++pos_;
      }
    }
    return out;
  }

  i32 SignedBits(u32 count) {
    if (count == 0)
      return 0;
    const u32 raw = Bits(count);
    const u32 sign = 1u << (count - 1);
    return (raw & sign) ? static_cast<i32>(raw) - static_cast<i32>(sign << 1)
                        : static_cast<i32>(raw);
  }

  // 16.16 signed fixed point packed into `count` bits.
  f32 FixedBits(u32 count) {
    return static_cast<f32>(SignedBits(count)) / 65536.0f;
  }

  Rect ReadRect();
  Matrix ReadMatrix();
  ColorTransform ReadColorTransform(bool with_alpha);
  Rgba ReadRgb();
  Rgba ReadRgba();

 private:
  ByteSpan data_;
  mem_size pos_ = 0;
  u32 bit_ = 0;
  bool ok_ = true;
};

}  // namespace rx::swf

#endif  // RECREATION_SWF_TYPES_H_

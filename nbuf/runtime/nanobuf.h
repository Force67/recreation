// nanobuf C++ runtime. Header-only, C++17, no dependencies.
//
// Mirrors the Rust runtime in crates/nanobuf: a bounds-checked zero-copy
// View, a single-buffer Writer with identical padding rules, and a proto3
// codec for migration mode. All multi-byte values are little-endian; loads
// and stores go through std::memcpy, never reinterpret_cast.
//
// No exceptions: fallible reads return std::optional, bool, or
// nanobuf::Opt (which distinguishes an absent field from a corrupt buffer).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nanobuf {

// Little-endian scalar codec. The arithmetic specialization assumes a
// little-endian host, like every supported target.
template <typename T, typename Enable = void>
struct ScalarCodec;

template <typename T>
struct ScalarCodec<T, std::enable_if_t<std::is_arithmetic_v<T>>> {
  static constexpr size_t kSize = sizeof(T);

  static T Read(const uint8_t* p) {
    if constexpr (std::is_same_v<T, bool>) {
      return *p != 0;
    } else {
      T value;
      std::memcpy(&value, p, sizeof(T));
      return value;
    }
  }

  static void Store(uint8_t* p, T value) {
    if constexpr (std::is_same_v<T, bool>) {
      *p = value ? 1 : 0;
    } else {
      std::memcpy(p, &value, sizeof(T));
    }
  }
};

// Generated open enums are structs exposing an integral `value` member.
template <typename T>
struct ScalarCodec<T, std::void_t<decltype(std::declval<T>().value)>> {
  using Backing = decltype(std::declval<T>().value);
  static constexpr size_t kSize = sizeof(Backing);

  static T Read(const uint8_t* p) { return T{ScalarCodec<Backing>::Read(p)}; }
  static void Store(uint8_t* p, T value) {
    ScalarCodec<Backing>::Store(p, value.value);
  }
};

template <typename T>
T LoadLe(const uint8_t* p) {
  return ScalarCodec<T>::Read(p);
}

template <typename T>
void StoreLe(uint8_t* p, T value) {
  ScalarCodec<T>::Store(p, value);
}

inline uint32_t BitsOf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

inline uint64_t BitsOf(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, 8);
  return bits;
}

inline float FloatFromBits(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

inline double DoubleFromBits(uint64_t bits) {
  double value;
  std::memcpy(&value, &bits, 8);
  return value;
}

// An IEEE 754 binary16 value, stored as its raw bit pattern. C++17 has no
// half type, so conversions go through float; narrowing rounds to nearest,
// ties to even, exactly like the other language runtimes.
//
// Equality compares bits: -0.0 != 0.0, and NaN == NaN when the payloads
// match. Compare via ToFloat() for numeric semantics.
struct Float16 {
  uint16_t bits = 0;

  // Rounds to the nearest representable value, ties to even. Values
  // beyond the binary16 range become infinity.
  static Float16 FromFloat(float value) {
    uint32_t raw = BitsOf(value);
    uint16_t sign = static_cast<uint16_t>((raw >> 16) & 0x8000);
    int exp = static_cast<int>((raw >> 23) & 0xff);
    uint32_t frac = raw & 0x007fffff;

    if (exp == 255) {
      if (frac == 0) {
        return Float16{static_cast<uint16_t>(sign | 0x7c00)};
      }
      // NaN: keep the top payload bits, force the quiet bit so the
      // payload can never read back as infinity.
      return Float16{static_cast<uint16_t>(sign | 0x7c00 | 0x0200 |
                                           static_cast<uint16_t>(frac >> 13))};
    }
    int unbiased = exp - 127;
    if (unbiased >= 16) {
      return Float16{static_cast<uint16_t>(sign | 0x7c00)};
    }
    if (unbiased >= -14) {
      // Normal result: drop 13 fraction bits with round-to-nearest-even.
      // A carry out of the fraction bumps the exponent, which also turns
      // 65520.0 and up into infinity.
      uint32_t half = (static_cast<uint32_t>(unbiased + 15) << 10) | (frac >> 13);
      uint32_t dropped = frac & 0x1fff;
      if (dropped > 0x1000 || (dropped == 0x1000 && (half & 1) != 0)) {
        half += 1;
      }
      return Float16{static_cast<uint16_t>(sign | half)};
    }
    if (unbiased >= -25 && exp != 0) {
      // Subnormal result; a round-up carry into bit 10 lands on the
      // smallest normal value, which is exactly right.
      uint32_t mantissa = frac | 0x00800000;
      uint32_t shift = static_cast<uint32_t>(-1 - unbiased);
      uint32_t dropped = mantissa & ((1u << shift) - 1);
      uint32_t halfway = 1u << (shift - 1);
      uint32_t half = mantissa >> shift;
      if (dropped > halfway || (dropped == halfway && (half & 1) != 0)) {
        half += 1;
      }
      return Float16{static_cast<uint16_t>(sign | half)};
    }
    // Too small for binary16 (including all float subnormals).
    return Float16{sign};
  }

  // Exact widening conversion.
  float ToFloat() const {
    uint32_t sign = static_cast<uint32_t>(bits & 0x8000) << 16;
    uint32_t exp = (bits >> 10) & 0x1f;
    uint32_t frac = bits & 0x03ff;
    uint32_t raw;
    if (exp == 0) {
      if (frac == 0) {
        raw = sign;
      } else {
        // Subnormal: renormalize under the wider exponent. The fraction's
        // top bit at position `p` becomes implicit.
        uint32_t p = 31;
        while ((frac & (1u << p)) == 0) {
          --p;
        }
        raw = sign | ((103 + p) << 23) | ((frac << (23 - p)) & 0x007fffff);
      }
    } else if (exp == 31) {
      raw = sign | 0x7f800000 | (frac << 13);
    } else {
      raw = sign | ((exp + 112) << 23) | (frac << 13);
    }
    return FloatFromBits(raw);
  }
};

inline bool operator==(Float16 a, Float16 b) { return a.bits == b.bits; }
inline bool operator!=(Float16 a, Float16 b) { return a.bits != b.bits; }

template <>
struct ScalarCodec<Float16> {
  static constexpr size_t kSize = 2;

  static Float16 Read(const uint8_t* p) {
    return Float16{ScalarCodec<uint16_t>::Read(p)};
  }
  static void Store(uint8_t* p, Float16 value) {
    ScalarCodec<uint16_t>::Store(p, value.bits);
  }
};

inline bool IsValidUtf8(const uint8_t* data, size_t size) {
  size_t i = 0;
  while (i < size) {
    uint8_t lead = data[i];
    if (lead < 0x80) {
      i += 1;
      continue;
    }
    size_t len;
    uint32_t code;
    uint32_t min;
    if ((lead & 0xe0) == 0xc0) {
      len = 2;
      code = lead & 0x1f;
      min = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
      len = 3;
      code = lead & 0x0f;
      min = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
      len = 4;
      code = lead & 0x07;
      min = 0x10000;
    } else {
      return false;
    }
    if (size - i < len) {
      return false;
    }
    for (size_t j = 1; j < len; ++j) {
      uint8_t cont = data[i + j];
      if ((cont & 0xc0) != 0x80) {
        return false;
      }
      code = (code << 6) | (cont & 0x3f);
    }
    if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
      return false;
    }
    i += len;
  }
  return true;
}

// Borrowed byte range into a serialized message.
struct BytesView {
  const uint8_t* data = nullptr;
  size_t size = 0;

  std::vector<uint8_t> ToVector() const {
    return std::vector<uint8_t>(data, data + size);
  }
};

inline bool operator==(BytesView a, BytesView b) {
  return a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}

inline bool operator!=(BytesView a, BytesView b) { return !(a == b); }

// Result of reading an optional heap field: `!ok()` means the buffer is
// corrupt, an ok-but-empty result means the field is absent.
template <typename T>
class Opt {
 public:
  static Opt Corrupt() { return Opt(false, std::nullopt); }
  static Opt Absent() { return Opt(true, std::nullopt); }
  static Opt Of(T value) { return Opt(true, std::move(value)); }

  bool ok() const { return ok_; }
  bool has_value() const { return value_.has_value(); }
  const T& value() const { return *value_; }

 private:
  Opt(bool ok, std::optional<T> value) : ok_(ok), value_(std::move(value)) {}

  bool ok_;
  std::optional<T> value_;
};

// Zero-copy list of fixed-width elements.
template <typename T>
class ScalarList {
 public:
  ScalarList() = default;
  ScalarList(const uint8_t* payload, size_t count)
      : payload_(payload), count_(count) {}

  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

  T operator[](size_t index) const {
    return ScalarCodec<T>::Read(payload_ + index * ScalarCodec<T>::kSize);
  }

  class Iterator {
   public:
    explicit Iterator(const uint8_t* p) : p_(p) {}
    T operator*() const { return ScalarCodec<T>::Read(p_); }
    Iterator& operator++() {
      p_ += ScalarCodec<T>::kSize;
      return *this;
    }
    bool operator==(Iterator other) const { return p_ == other.p_; }
    bool operator!=(Iterator other) const { return p_ != other.p_; }

   private:
    const uint8_t* p_;
  };

  Iterator begin() const { return Iterator(payload_); }
  Iterator end() const {
    return Iterator(payload_ + count_ * ScalarCodec<T>::kSize);
  }

  std::vector<T> ToVector() const {
    std::vector<T> out;
    out.reserve(count_);
    for (size_t i = 0; i < count_; ++i) {
      out.push_back((*this)[i]);
    }
    return out;
  }

 private:
  const uint8_t* payload_ = nullptr;
  size_t count_ = 0;
};

class OffsetList;
class StrList;
class BytesList;

// Zero-copy view over one serialized message.
//
// Scalar accessors are infallible: anything past the writer's fixed
// section reads as zero, which is how old data yields defaults for new
// fields. Heap accessors bounds-check the referenced entry and report
// corrupt input through Opt / std::optional.
class View {
 public:
  View() = default;

  static std::optional<View> Parse(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 2) {
      return std::nullopt;
    }
    uint16_t fixed_len = LoadLe<uint16_t>(data);
    if (fixed_len < 2 || fixed_len > size) {
      return std::nullopt;
    }
    return View(data, size, fixed_len);
  }

  // The raw message bytes, suitable for re-framing or forwarding.
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

  // Reads a fixed-section scalar, or zero when the writer's schema was
  // older and the slot does not exist.
  template <typename T>
  T ScalarAt(size_t offset) const {
    if (offset + ScalarCodec<T>::kSize > fixed_len_) {
      return T{};
    }
    return ScalarCodec<T>::Read(data_ + offset);
  }

  bool BitAt(size_t byte, uint8_t bit) const {
    return byte < fixed_len_ && ((data_[byte] >> bit) & 1) != 0;
  }

  // `bytes` or optional-`bytes` field. Absent() means the slot is empty.
  Opt<BytesView> BytesAt(size_t slot) const {
    std::optional<size_t> entry = OffsetAt(slot);
    if (!entry.has_value()) {
      return Opt<BytesView>::Absent();
    }
    std::optional<BytesView> payload = EntryAt(*entry);
    if (!payload.has_value()) {
      return Opt<BytesView>::Corrupt();
    }
    return Opt<BytesView>::Of(*payload);
  }

  // `string` or optional-`string` field; validates UTF-8.
  Opt<std::string_view> StrAt(size_t slot) const {
    Opt<BytesView> bytes = BytesAt(slot);
    if (!bytes.ok()) {
      return Opt<std::string_view>::Corrupt();
    }
    if (!bytes.has_value()) {
      return Opt<std::string_view>::Absent();
    }
    BytesView payload = bytes.value();
    if (!IsValidUtf8(payload.data, payload.size)) {
      return Opt<std::string_view>::Corrupt();
    }
    return Opt<std::string_view>::Of(std::string_view(
        reinterpret_cast<const char*>(payload.data), payload.size));
  }

  // Message-typed field. Absent() means the slot is empty.
  Opt<View> MessageAt(size_t slot) const {
    Opt<BytesView> bytes = BytesAt(slot);
    if (!bytes.ok()) {
      return Opt<View>::Corrupt();
    }
    if (!bytes.has_value()) {
      return Opt<View>::Absent();
    }
    std::optional<View> child = Parse(bytes.value().data, bytes.value().size);
    if (!child.has_value()) {
      return Opt<View>::Corrupt();
    }
    return Opt<View>::Of(*child);
  }

  // `list<scalar>` or `list<enum>` field. Absent reads as empty.
  template <typename T>
  std::optional<ScalarList<T>> ScalarListAt(size_t slot) const {
    std::optional<size_t> entry = OffsetAt(slot);
    if (!entry.has_value()) {
      return ScalarList<T>();
    }
    if (size_ < 4 || *entry > size_ - 4) {
      return std::nullopt;
    }
    size_t count = LoadLe<uint32_t>(data_ + *entry);
    size_t available = size_ - *entry - 4;
    if (count > available / ScalarCodec<T>::kSize) {
      return std::nullopt;
    }
    return ScalarList<T>(data_ + *entry + 4, count);
  }

  // `list<string|bytes|message>` field. Absent reads as empty.
  std::optional<OffsetList> OffsetListAt(size_t slot) const;
  std::optional<StrList> StrListAt(size_t slot) const;
  std::optional<BytesList> BytesListAt(size_t slot) const;

 private:
  View(const uint8_t* data, size_t size, uint16_t fixed_len)
      : data_(data), size_(size), fixed_len_(fixed_len) {}

  // Resolves an offset slot to a message-relative heap offset.
  // Empty when the slot is zero or beyond the writer's fixed section.
  std::optional<size_t> OffsetAt(size_t slot) const {
    if (slot + 4 > fixed_len_) {
      return std::nullopt;
    }
    uint32_t offset = LoadLe<uint32_t>(data_ + slot);
    if (offset == 0) {
      return std::nullopt;
    }
    return offset;
  }

  // Reads the `[len: u32][payload]` heap entry at `entry`.
  std::optional<BytesView> EntryAt(size_t entry) const {
    if (size_ < 4 || entry > size_ - 4) {
      return std::nullopt;
    }
    uint32_t len = LoadLe<uint32_t>(data_ + entry);
    if (len > size_ - entry - 4) {
      return std::nullopt;
    }
    return BytesView{data_ + entry + 4, len};
  }

  friend class OffsetList;

  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t fixed_len_ = 0;
};

// List of heap entries (strings, bytes, or messages), resolved lazily.
class OffsetList {
 public:
  OffsetList() = default;
  OffsetList(View view, const uint8_t* offsets, size_t count)
      : view_(view), offsets_(offsets), count_(count) {}

  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

  // Raw payload of element `index`; empty on corrupt input.
  std::optional<BytesView> BytesAt(size_t index) const {
    if (index >= count_) {
      return std::nullopt;
    }
    uint32_t offset = LoadLe<uint32_t>(offsets_ + index * 4);
    if (offset == 0) {
      return std::nullopt;
    }
    return view_.EntryAt(offset);
  }

  std::optional<std::string_view> StrAt(size_t index) const {
    std::optional<BytesView> payload = BytesAt(index);
    if (!payload.has_value() || !IsValidUtf8(payload->data, payload->size)) {
      return std::nullopt;
    }
    return std::string_view(reinterpret_cast<const char*>(payload->data),
                            payload->size);
  }

  std::optional<View> MessageAt(size_t index) const {
    std::optional<BytesView> payload = BytesAt(index);
    if (!payload.has_value()) {
      return std::nullopt;
    }
    return View::Parse(payload->data, payload->size);
  }

 private:
  View view_;
  const uint8_t* offsets_ = nullptr;
  size_t count_ = 0;
};

inline std::optional<OffsetList> View::OffsetListAt(size_t slot) const {
  std::optional<size_t> entry = OffsetAt(slot);
  if (!entry.has_value()) {
    return OffsetList();
  }
  if (size_ < 4 || *entry > size_ - 4) {
    return std::nullopt;
  }
  size_t count = LoadLe<uint32_t>(data_ + *entry);
  if (count > (size_ - *entry - 4) / 4) {
    return std::nullopt;
  }
  return OffsetList(*this, data_ + *entry + 4, count);
}

// Typed `list<string>` reader.
class StrList {
 public:
  StrList() = default;
  explicit StrList(OffsetList list) : list_(list) {}

  size_t size() const { return list_.size(); }
  bool empty() const { return list_.empty(); }

  std::optional<std::string_view> Get(size_t index) const {
    return list_.StrAt(index);
  }

  std::optional<std::vector<std::string>> ToVector() const {
    std::vector<std::string> out;
    out.reserve(size());
    for (size_t i = 0; i < size(); ++i) {
      std::optional<std::string_view> item = Get(i);
      if (!item.has_value()) {
        return std::nullopt;
      }
      out.emplace_back(*item);
    }
    return out;
  }

 private:
  OffsetList list_;
};

// Typed `list<bytes>` reader.
class BytesList {
 public:
  BytesList() = default;
  explicit BytesList(OffsetList list) : list_(list) {}

  size_t size() const { return list_.size(); }
  bool empty() const { return list_.empty(); }

  std::optional<BytesView> Get(size_t index) const {
    return list_.BytesAt(index);
  }

  std::optional<std::vector<std::vector<uint8_t>>> ToVector() const {
    std::vector<std::vector<uint8_t>> out;
    out.reserve(size());
    for (size_t i = 0; i < size(); ++i) {
      std::optional<BytesView> item = Get(i);
      if (!item.has_value()) {
        return std::nullopt;
      }
      out.push_back(item->ToVector());
    }
    return out;
  }

 private:
  OffsetList list_;
};

inline std::optional<StrList> View::StrListAt(size_t slot) const {
  std::optional<OffsetList> list = OffsetListAt(slot);
  if (!list.has_value()) {
    return std::nullopt;
  }
  return StrList(*list);
}

inline std::optional<BytesList> View::BytesListAt(size_t slot) const {
  std::optional<OffsetList> list = OffsetListAt(slot);
  if (!list.has_value()) {
    return std::nullopt;
  }
  return BytesList(*list);
}

// Typed `list<message>` reader yielding generated view types, which are
// constructible from a raw View.
template <typename T>
class MessageList {
 public:
  MessageList() = default;
  explicit MessageList(OffsetList list) : list_(list) {}

  size_t size() const { return list_.size(); }
  bool empty() const { return list_.empty(); }

  std::optional<T> Get(size_t index) const {
    std::optional<View> view = list_.MessageAt(index);
    if (!view.has_value()) {
      return std::nullopt;
    }
    return T(*view);
  }

 private:
  OffsetList list_;
};

// Single-buffer message writer.
//
// Generated WriteTo implementations call Begin to reserve their fixed
// section, patch field slots, and append heap entries. Nested messages are
// written depth-first into the same buffer.
class Writer {
 public:
  std::vector<uint8_t> TakeBuffer() { return std::move(buf_); }

  // Starts a message at the current position: writes the header and
  // zero-fills the rest of the fixed section.
  void Begin(uint16_t fixed_len) {
    base_ = buf_.size();
    buf_.resize(base_ + fixed_len, 0);
    StoreLe<uint16_t>(buf_.data() + base_, fixed_len);
  }

  // Writes a scalar into the fixed section.
  template <typename T>
  void PutScalar(uint16_t offset, T value) {
    ScalarCodec<T>::Store(buf_.data() + base_ + offset, value);
  }

  void PutBit(uint16_t byte, uint8_t bit, bool value) {
    if (value) {
      buf_[base_ + byte] |= static_cast<uint8_t>(1u << bit);
    }
  }

  // Appends a `[len][payload]` heap entry and returns its
  // message-relative offset.
  uint32_t HeapBytes(const uint8_t* payload, size_t len) {
    size_t entry = buf_.size();
    AppendU32(static_cast<uint32_t>(len));
    buf_.insert(buf_.end(), payload, payload + len);
    return static_cast<uint32_t>(entry - base_);
  }

  uint32_t HeapBytes(const std::vector<uint8_t>& payload) {
    return HeapBytes(payload.data(), payload.size());
  }

  uint32_t HeapString(std::string_view payload) {
    return HeapBytes(reinterpret_cast<const uint8_t*>(payload.data()),
                     payload.size());
  }

  // Appends a nested message entry; `write_body` serializes the child.
  // Returns the entry's message-relative offset.
  template <typename F>
  uint32_t HeapMessage(F&& write_body) {
    // Message payloads are 8-aligned relative to the enclosing message.
    PadPayloadTo(8);
    size_t entry = buf_.size();
    uint32_t offset = static_cast<uint32_t>(entry - base_);
    AppendU32(0);
    size_t outer_base = base_;
    size_t body_start = buf_.size();
    write_body(*this);
    StoreLe<uint32_t>(buf_.data() + entry,
                      static_cast<uint32_t>(buf_.size() - body_start));
    base_ = outer_base;
    return offset;
  }

  // Patches an offset slot in the fixed section.
  void SetSlot(uint16_t slot, uint32_t heap_offset) {
    StoreLe<uint32_t>(buf_.data() + base_ + slot, heap_offset);
  }

  // `string`/`bytes` field: writes the entry unless the value is empty
  // and `force` is false (absent and empty read identically then).
  void PutBytes(uint16_t slot, const std::vector<uint8_t>& payload, bool force) {
    if (payload.empty() && !force) {
      return;
    }
    SetSlot(slot, HeapBytes(payload));
  }

  void PutString(uint16_t slot, std::string_view payload, bool force) {
    if (payload.empty() && !force) {
      return;
    }
    SetSlot(slot, HeapString(payload));
  }

  // Message-typed field.
  template <typename F>
  void PutMessage(uint16_t slot, F&& write_body) {
    SetSlot(slot, HeapMessage(std::forward<F>(write_body)));
  }

  // `list<scalar>` / `list<enum>` field. Empty lists are absent.
  template <typename T>
  void PutScalarList(uint16_t slot, const std::vector<T>& items) {
    if (items.empty()) {
      return;
    }
    PadPayloadTo(ScalarCodec<T>::kSize);
    size_t entry = buf_.size();
    AppendU32(static_cast<uint32_t>(items.size()));
    for (size_t i = 0; i < items.size(); ++i) {
      T value = items[i];
      size_t at = buf_.size();
      buf_.resize(at + ScalarCodec<T>::kSize);
      ScalarCodec<T>::Store(buf_.data() + at, value);
    }
    SetSlot(slot, static_cast<uint32_t>(entry - base_));
  }

  // `list<string|bytes|message>` field: writes the offset array, then one
  // entry per item via `write_item`, which returns each entry's
  // message-relative offset (e.g. from HeapBytes).
  template <typename T, typename F>
  void PutOffsetList(uint16_t slot, const std::vector<T>& items, F&& write_item) {
    if (items.empty()) {
      return;
    }
    size_t entry = buf_.size();
    AppendU32(static_cast<uint32_t>(items.size()));
    size_t table = buf_.size();
    buf_.resize(table + items.size() * 4, 0);
    for (size_t i = 0; i < items.size(); ++i) {
      uint32_t offset = write_item(*this, items[i]);
      StoreLe<uint32_t>(buf_.data() + table + i * 4, offset);
    }
    SetSlot(slot, static_cast<uint32_t>(entry - base_));
  }

 private:
  void AppendU32(uint32_t value) {
    size_t at = buf_.size();
    buf_.resize(at + 4);
    StoreLe<uint32_t>(buf_.data() + at, value);
  }

  // Pads so the next entry's payload (after its 4-byte length/count
  // prefix) lands `align`-aligned relative to the message start.
  void PadPayloadTo(size_t align) {
    size_t payload_pos = buf_.size() - base_ + 4;
    size_t target = (payload_pos + align - 1) / align * align;
    buf_.resize(buf_.size() + (target - payload_pos), 0);
  }

  std::vector<uint8_t> buf_;
  // Start of the current message; heap offsets are relative to it.
  size_t base_ = 0;
};

// Proto3 wire format codec, used only in migration mode.
namespace proto {

// Proto3 wire types. Groups (3 and 4) are not supported.
enum class WireType : uint32_t {
  kVarint = 0,
  kI64 = 1,
  kLen = 2,
  kI32 = 5,
};

// One decoded field value.
struct WireValue {
  WireType type = WireType::kVarint;
  uint64_t scalar = 0;            // varint, i64, or i32 payload
  const uint8_t* data = nullptr;  // len payload
  size_t size = 0;
};

inline bool ReadVarint(const uint8_t* buf, size_t size, size_t* pos,
                       uint64_t* value) {
  uint64_t result = 0;
  for (int i = 0; i < 10; ++i) {
    if (*pos >= size) {
      return false;
    }
    uint8_t byte = buf[(*pos)++];
    // The tenth byte may only carry the final bit of a u64.
    if (i == 9 && byte > 1) {
      return false;
    }
    result |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
  }
  return false;
}

inline void WriteVarint(std::vector<uint8_t>& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>(value) | 0x80);
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

inline uint64_t ZigZagEncode(int64_t value) {
  return (static_cast<uint64_t>(value) << 1) ^ (value < 0 ? ~uint64_t{0} : 0);
}

inline int64_t ZigZagDecode(uint64_t value) {
  return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}

// Iterates `(field_number, value)` pairs of one proto3 message.
// Unknown fields are simply skipped by ignoring the yielded value.
class FieldReader {
 public:
  enum class Status { kField, kDone, kError };

  FieldReader(const uint8_t* buf, size_t size) : buf_(buf), size_(size) {}

  Status Next(uint32_t* field, WireValue* value) {
    if (pos_ >= size_) {
      return Status::kDone;
    }
    uint64_t tag;
    if (!ReadVarint(buf_, size_, &pos_, &tag)) {
      return Status::kError;
    }
    uint64_t number = tag >> 3;
    if (number == 0 || number > 0xffffffffu) {
      return Status::kError;
    }
    *field = static_cast<uint32_t>(number);
    switch (tag & 7) {
      case 0:
        value->type = WireType::kVarint;
        return ReadVarint(buf_, size_, &pos_, &value->scalar) ? Status::kField
                                                              : Status::kError;
      case 1:
        value->type = WireType::kI64;
        return ReadFixed(8, value);
      case 2: {
        uint64_t len;
        if (!ReadVarint(buf_, size_, &pos_, &len) || len > size_ - pos_) {
          return Status::kError;
        }
        value->type = WireType::kLen;
        value->data = buf_ + pos_;
        value->size = static_cast<size_t>(len);
        pos_ += static_cast<size_t>(len);
        return Status::kField;
      }
      case 5:
        value->type = WireType::kI32;
        return ReadFixed(4, value);
      default:
        return Status::kError;
    }
  }

 private:
  Status ReadFixed(size_t width, WireValue* value) {
    if (width > size_ - pos_) {
      return Status::kError;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < width; ++i) {
      result |= static_cast<uint64_t>(buf_[pos_ + i]) << (i * 8);
    }
    value->scalar = result;
    pos_ += width;
    return Status::kField;
  }

  const uint8_t* buf_;
  size_t size_;
  size_t pos_ = 0;
};

// Helpers used by generated DecodeProto3 code: each expects one wire shape
// and fails otherwise.
inline bool ExpectVarint(const WireValue& value, uint64_t* out) {
  if (value.type != WireType::kVarint) {
    return false;
  }
  *out = value.scalar;
  return true;
}

inline bool ExpectI32(const WireValue& value, uint32_t* out) {
  if (value.type != WireType::kI32) {
    return false;
  }
  *out = static_cast<uint32_t>(value.scalar);
  return true;
}

inline bool ExpectI64(const WireValue& value, uint64_t* out) {
  if (value.type != WireType::kI64) {
    return false;
  }
  *out = value.scalar;
  return true;
}

inline bool ExpectLen(const WireValue& value, const uint8_t** data,
                      size_t* size) {
  if (value.type != WireType::kLen) {
    return false;
  }
  *data = value.data;
  *size = value.size;
  return true;
}

inline bool ExpectString(const WireValue& value, std::string* out) {
  const uint8_t* data;
  size_t size;
  if (!ExpectLen(value, &data, &size) || !IsValidUtf8(data, size)) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(data), size);
  return true;
}

inline bool ExpectBytes(const WireValue& value, std::vector<uint8_t>* out) {
  const uint8_t* data;
  size_t size;
  if (!ExpectLen(value, &data, &size)) {
    return false;
  }
  out->assign(data, data + size);
  return true;
}

// Reads a packed varint list payload, or a single unpacked element.
template <typename F>
bool ReadPackedVarints(const WireValue& value, F&& push) {
  if (value.type == WireType::kVarint) {
    push(value.scalar);
    return true;
  }
  if (value.type != WireType::kLen) {
    return false;
  }
  size_t pos = 0;
  while (pos < value.size) {
    uint64_t v;
    if (!ReadVarint(value.data, value.size, &pos, &v)) {
      return false;
    }
    push(v);
  }
  return true;
}

// Reads a packed fixed32 list payload, or a single unpacked element.
template <typename F>
bool ReadPackedFixed32(const WireValue& value, F&& push) {
  if (value.type == WireType::kI32) {
    push(static_cast<uint32_t>(value.scalar));
    return true;
  }
  if (value.type != WireType::kLen || value.size % 4 != 0) {
    return false;
  }
  for (size_t i = 0; i < value.size; i += 4) {
    push(LoadLe<uint32_t>(value.data + i));
  }
  return true;
}

// Reads a packed fixed64 list payload, or a single unpacked element.
template <typename F>
bool ReadPackedFixed64(const WireValue& value, F&& push) {
  if (value.type == WireType::kI64) {
    push(value.scalar);
    return true;
  }
  if (value.type != WireType::kLen || value.size % 8 != 0) {
    return false;
  }
  for (size_t i = 0; i < value.size; i += 8) {
    push(LoadLe<uint64_t>(value.data + i));
  }
  return true;
}

inline void WriteTag(std::vector<uint8_t>& out, uint32_t field, WireType type) {
  WriteVarint(out, (static_cast<uint64_t>(field) << 3) |
                       static_cast<uint64_t>(type));
}

inline void WriteVarintField(std::vector<uint8_t>& out, uint32_t field,
                             uint64_t value) {
  WriteTag(out, field, WireType::kVarint);
  WriteVarint(out, value);
}

inline void WriteFixed32(std::vector<uint8_t>& out, uint32_t value) {
  size_t at = out.size();
  out.resize(at + 4);
  StoreLe<uint32_t>(out.data() + at, value);
}

inline void WriteFixed64(std::vector<uint8_t>& out, uint64_t value) {
  size_t at = out.size();
  out.resize(at + 8);
  StoreLe<uint64_t>(out.data() + at, value);
}

inline void WriteI32Field(std::vector<uint8_t>& out, uint32_t field,
                          uint32_t value) {
  WriteTag(out, field, WireType::kI32);
  WriteFixed32(out, value);
}

inline void WriteI64Field(std::vector<uint8_t>& out, uint32_t field,
                          uint64_t value) {
  WriteTag(out, field, WireType::kI64);
  WriteFixed64(out, value);
}

inline void WriteLenField(std::vector<uint8_t>& out, uint32_t field,
                          const uint8_t* payload, size_t size) {
  WriteTag(out, field, WireType::kLen);
  WriteVarint(out, size);
  out.insert(out.end(), payload, payload + size);
}

inline void WriteLenField(std::vector<uint8_t>& out, uint32_t field,
                          std::string_view payload) {
  WriteLenField(out, field,
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size());
}

inline void WriteLenField(std::vector<uint8_t>& out, uint32_t field,
                          const std::vector<uint8_t>& payload) {
  WriteLenField(out, field, payload.data(), payload.size());
}

// Writes a length-delimited field whose payload is produced by `body`.
// Used for nested messages and packed lists.
template <typename F>
void WriteLenFieldWith(std::vector<uint8_t>& out, uint32_t field, F&& body) {
  std::vector<uint8_t> payload;
  body(payload);
  WriteLenField(out, field, payload);
}

}  // namespace proto
}  // namespace nanobuf

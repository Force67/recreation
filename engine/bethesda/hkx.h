#ifndef RECREATION_BETHESDA_HKX_H_
#define RECREATION_BETHESDA_HKX_H_

// Havok binary packfile (.hkx) reader for the hk_2010.2.0 content version
// Skyrim SE ships (64-bit pointers; LE-era 32-bit packfiles also parse).
// A packfile is a serialized object graph: a __classnames__ section naming
// every serialized type, a __data__ section holding the raw C++ object
// images, and three fixup tables gluing them together (local: intra-section
// pointers, global: cross-section object references, virtual: "object at
// offset X is an instance of class Y"). This reader indexes the fixups and
// exposes a typed cursor over the data image; the class-specific decoding
// (skeletons, rigid bodies, shapes) lives in hkx_physics.{h,cc}.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/types.h"

namespace rec::bethesda {

struct HkxObject {
  u64 offset = 0;               // into the data image
  std::string_view class_name;  // backed by HkxFile::classnames_
};

class HkxFile {
 public:
  static constexpr u64 kNull = ~0ull;

  // Parses the packfile container. Returns std::nullopt when the magic,
  // version, or section layout is not a packfile this reader understands.
  static std::optional<HkxFile> Parse(const u8* bytes, size_t size);

  // hk_2010.2.0-r1 etc.
  const std::string& content_version() const { return content_version_; }
  u32 pointer_size() const { return pointer_size_; }

  // Every serialized object (from the virtual fixup table), in file order.
  // The first object is the hkRootLevelContainer.
  const std::vector<HkxObject>& objects() const { return objects_; }
  // Class name of the object AT `offset`, empty when offset is not an object
  // start (e.g. an interior array element).
  std::string_view class_of(u64 offset) const;

  // --- typed reads from the data image (bounds-checked, zero on overrun) ---
  u8 U8(u64 offset) const;
  u16 U16(u64 offset) const;
  u32 U32(u64 offset) const;
  u64 U64(u64 offset) const;
  i16 I16(u64 offset) const;
  f32 F32(u64 offset) const;

  // Follows the pointer stored at `offset` through the fixup tables.
  // Returns kNull for null pointers (no fixup entry).
  u64 Pointer(u64 offset) const;
  // C string at the offset a pointer at `offset` resolves to ("" for null).
  std::string_view CString(u64 offset) const;

  // hkArray<T>: { T* data; int size; int capacityAndFlags; }. `offset` is the
  // array member's offset; returns the element base (kNull when empty/null)
  // and writes the count.
  u64 Array(u64 offset, u32* count) const;

  size_t data_size() const { return data_.size(); }
  const u8* data() const { return data_.data(); }

 private:
  std::vector<u8> data_;         // __data__ section image
  std::string classnames_;       // __classnames__ section image (owns the names)
  std::string content_version_;
  u32 pointer_size_ = 8;
  std::unordered_map<u64, u64> pointers_;   // local+global fixups: from -> to
  std::unordered_map<u64, u32> object_at_;  // offset -> index into objects_
  std::vector<HkxObject> objects_;
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_HKX_H_

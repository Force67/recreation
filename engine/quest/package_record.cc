#include "quest/package_record.h"

#include <cstring>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rx::quest {
namespace {

constexpr u32 kPkdt = FourCc('P', 'K', 'D', 'T');
constexpr u32 kAnam = FourCc('A', 'N', 'A', 'M');
constexpr u32 kPldt = FourCc('P', 'L', 'D', 'T');
constexpr u32 kPtda = FourCc('P', 'T', 'D', 'A');

// PLDT (Location) type enum, as stored in the struct's first u32.
enum PldtType : u32 {
  kPldtNearReference = 0,   // data = REFR form id
  kPldtInCell = 1,          // data = CELL form id
  kPldtNearCurrent = 2,     // data = 0 (the actor's current location)
  kPldtNearEditor = 3,      // data = 0
  kPldtLinkedRef = 6,       // data = keyword form id or 0
  kPldtReferenceAlias = 8,  // data = quest alias index
  kPldtPackageStart = 12,   // data = 0
};

// PTDA (Target) type enum, as stored in the struct's first i32.
enum PtdaType : i32 {
  kPtdaSpecificReference = 0,  // data = REFR form id
  kPtdaObjectId = 1,           // data = base form id
  kPtdaReferenceAlias = 2,     // data = quest alias index
  kPtdaLinkedRef = 3,          // data = keyword form id or 0
  kPtdaSelf = 6,               // data = 0
};

template <typename T>
T ReadAt(const bethesda::Subrecord& sub, size_t offset) {
  T value{};
  if (sub.data.size() >= offset + sizeof(T))
    std::memcpy(&value, sub.data.data() + offset, sizeof(T));
  return value;
}

// A package destination is a "travel" destination when it sends the actor to a
// place distinct from where it stands. A self/current/none target is the actor
// holding position, which is not travel.
bool IsTravelTarget(PackageTarget::Kind kind) {
  switch (kind) {
    case PackageTarget::Kind::kReference:
    case PackageTarget::Kind::kAlias:
    case PackageTarget::Kind::kLocation:
      return true;
    case PackageTarget::Kind::kLinkedRef:
    case PackageTarget::Kind::kSelf:
    case PackageTarget::Kind::kNone:
      return false;
  }
  return false;
}

PackageDef ParseImpl(u64 handle, const bethesda::Record& record,
                     const bethesda::RecordStore* records) {
  PackageDef def;
  def.handle = handle;

  // PACK form ids inside the body are plugin relative. Resolve them against the
  // masters of the plugin that won this record; the handle's high word is its
  // plugin index. Without a store, references stay raw.
  u16 plugin = static_cast<u16>(handle >> 32);
  auto resolve = [&](u32 raw) -> u64 {
    if (raw == 0 || records == nullptr) return raw;
    return records->ResolveFrom(bethesda::RawFormId{raw}, plugin).packed();
  };

  if (const bethesda::Subrecord* pkdt = record.Find(kPkdt)) {
    def.flags = ReadAt<u32>(*pkdt, 0);
    def.type = ReadAt<u8>(*pkdt, 4);
  }

  // Walk the typed input run and take the first Location/Target struct. That
  // first destination is the package's primary target; later PLDT/PTDA entries
  // are secondary inputs (sandbox radius anchors, fallback points).
  bool have_target = false;
  for (const bethesda::Subrecord& sub : record.subrecords) {
    if (have_target) break;
    if (sub.type == kPldt) {
      u32 type = ReadAt<u32>(sub, 0);
      u32 data = ReadAt<u32>(sub, 4);
      f32 radius = ReadAt<f32>(sub, 8);
      PackageTarget& t = def.target;
      t.raw_kind = type;
      t.radius = radius;
      switch (type) {
        case kPldtNearReference:
        case kPldtInCell:
          t.kind = type == kPldtInCell ? PackageTarget::Kind::kLocation
                                       : PackageTarget::Kind::kReference;
          t.ref = resolve(data);
          break;
        case kPldtReferenceAlias:
          t.kind = PackageTarget::Kind::kAlias;
          t.alias = static_cast<i32>(data);
          break;
        case kPldtLinkedRef:
          t.kind = PackageTarget::Kind::kLinkedRef;
          t.ref = resolve(data);
          break;
        case kPldtNearCurrent:
          t.kind = PackageTarget::Kind::kSelf;
          break;
        case kPldtNearEditor:
        case kPldtPackageStart:
        default:
          t.kind = PackageTarget::Kind::kLocation;
          break;
      }
      have_target = true;
    } else if (sub.type == kPtda) {
      i32 type = ReadAt<i32>(sub, 0);
      u32 data = ReadAt<u32>(sub, 4);
      PackageTarget& t = def.target;
      t.raw_kind = static_cast<u32>(type);
      switch (type) {
        case kPtdaSpecificReference:
        case kPtdaObjectId:
          t.kind = PackageTarget::Kind::kReference;
          t.ref = resolve(data);
          break;
        case kPtdaReferenceAlias:
          t.kind = PackageTarget::Kind::kAlias;
          t.alias = static_cast<i32>(data);
          break;
        case kPtdaLinkedRef:
          t.kind = PackageTarget::Kind::kLinkedRef;
          t.ref = resolve(data);
          break;
        case kPtdaSelf:
          t.kind = PackageTarget::Kind::kSelf;
          break;
        default:
          t.kind = PackageTarget::Kind::kLocation;
          break;
      }
      have_target = true;
    }
  }

  def.is_travel = IsTravelTarget(def.target.kind);
  return def;
}

}  // namespace

PackageDef ParsePackageRecord(u64 handle, const bethesda::Record& record,
                              const bethesda::RecordStore& records) {
  return ParseImpl(handle, record, &records);
}

PackageDef ParsePackageRecord(u64 handle, const bethesda::Record& record) {
  return ParseImpl(handle, record, nullptr);
}

}  // namespace rx::quest

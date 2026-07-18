#include "bethesda/movement_type.h"

#include <cstring>

#include "bethesda/record.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

constexpr u32 kMovt = FourCc('M', 'O', 'V', 'T');
constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kSped = FourCc('S', 'P', 'E', 'D');

// Reads float #i (0-based) out of a SPED span, bounds-checked. SPED is a packed
// array of 11 little-endian float32 (44 bytes). Absent fields read as 0.
f32 ReadFloat(const ByteSpan& data, size_t index) {
  const size_t off = index * 4;
  if (off + 4 > data.size()) return 0.0f;
  f32 v;
  std::memcpy(&v, data.data() + off, 4);
  return v;
}

}  // namespace

int LoadMovementTypes(const RecordStore& records, std::unordered_map<u64, MovementType>* out) {
  if (!out) return 0;
  int count = 0;
  records.EachOfType(kMovt, [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    Record rec;
    if (!records.Parse(id, &rec)) return;
    MovementType mt;
    mt.form = id.packed();
    mt.editor_id = rec.GetString(kEdid);
    // SPED layout (UESP MOVT): 11 floats, cardinal walk/run pairs then rotations.
    //   0 left walk   1 left run    2 right walk  3 right run
    //   4 fwd walk    5 fwd run     6 back walk   7 back run
    //   8 rot-in-place walk  9 rot-in-place run  10 rot-while-moving run
    if (const Subrecord* sped = rec.Find(kSped); sped && sped->data.size() >= 4) {
      mt.left_walk = ReadFloat(sped->data, 0);
      mt.left_run = ReadFloat(sped->data, 1);
      mt.right_walk = ReadFloat(sped->data, 2);
      mt.right_run = ReadFloat(sped->data, 3);
      mt.forward_walk = ReadFloat(sped->data, 4);
      mt.forward_run = ReadFloat(sped->data, 5);
      mt.back_walk = ReadFloat(sped->data, 6);
      mt.back_run = ReadFloat(sped->data, 7);
      mt.rotate_in_place_walk = ReadFloat(sped->data, 8);
      mt.rotate_in_place_run = ReadFloat(sped->data, 9);
      mt.rotate_while_moving_run = ReadFloat(sped->data, 10);
      mt.has_speeds = true;
    }
    (*out)[mt.form] = std::move(mt);
    ++count;
  });
  return count;
}

const MovementType* FindMovementType(const std::unordered_map<u64, MovementType>& types,
                                     std::string_view editor_id) {
  for (const auto& [form, mt] : types) {
    if (mt.editor_id == editor_id) return &mt;
  }
  return nullptr;
}

}  // namespace rx::bethesda

#include "quest/ctda.h"

#include <cstring>

#include "bethesda/form_id.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rec::quest {
namespace {

// Maps a Creation Kit condition-function index to the native Func we can
// evaluate through a typed query. Intentionally partial and high-confidence:
// any unmapped id stays kRaw and routes to ConditionContext::EvalRaw, so adding
// an entry only widens declarative coverage, it never changes correctness.
Func MapFunction(u16 id) {
  switch (id) {
    case 1:
      return Func::kGetDistance;
    case 14:
      return Func::kGetActorValue;
    case 47:
      return Func::kGetItemCount;
    case 58:
      return Func::kGetStage;
    case 59:
      return Func::kGetStageDone;  // stage param2 of quest param1 has run
    case 72:
      return Func::kGetIsId;  // checks an actor's base form: the dialogue speaker gate
    default:
      return Func::kRaw;
  }
}

}  // namespace

bool ParseCtda(ByteSpan data, Comparison* out) {
  // Classic CTDA is 28 bytes; SE appends 4 bytes we ignore. Layout:
  //   0  control byte (operator in top 3 bits, flags in low 5)
  //   4  comparison value (float, or GLOB form id when "use global" is set)
  //   8  function index (u16)
  //   12 param1 (u32)   16 param2 (u32)
  //   20 run-on type (u32)   24 reference (u32)
  if (data.size() < 28) return false;
  const u8* p = data.data();
  auto U16 = [&](size_t o) {
    u16 v;
    std::memcpy(&v, p + o, sizeof(v));
    return v;
  };
  auto U32 = [&](size_t o) {
    u32 v;
    std::memcpy(&v, p + o, sizeof(v));
    return v;
  };
  auto F32 = [&](size_t o) {
    float v;
    std::memcpy(&v, p + o, sizeof(v));
    return v;
  };

  const u8 control = p[0];
  const u8 op = (control >> 5) & 0x07;
  const u8 flags = control & 0x1F;

  Comparison c;
  c.op = op <= static_cast<u8>(CompareOp::kLessOrEqual) ? static_cast<CompareOp>(op)
                                                        : CompareOp::kEqual;
  c.or_next = (flags & 0x01) != 0;
  if (flags & 0x04)  // use global: the value field is a GLOB form id
    c.global = U32(4);
  else
    c.value = F32(4);

  c.raw_function = U16(8);
  c.param1 = U32(12);
  c.param2 = U32(16);
  const u32 run_on = U32(20);
  c.run_on = run_on <= static_cast<u32>(RunOn::kEventData) ? static_cast<RunOn>(run_on)
                                                           : RunOn::kSubject;
  c.reference = U32(24);
  c.func = MapFunction(c.raw_function);

  *out = c;
  return true;
}

void ResolveConditionForms(ConditionList& conditions, const bethesda::RecordStore& records,
                           u16 plugin) {
  auto remap = [&](u64 raw) -> u64 {
    if (raw == 0) return 0;
    return records.ResolveFrom(bethesda::RawFormId{static_cast<u32>(raw)}, plugin).packed();
  };
  for (Comparison& c : conditions.comparisons) {
    if (c.global) c.global = remap(c.global);
    switch (c.func) {
      case Func::kGetStage:
      case Func::kGetStageDone:
      case Func::kGetItemCount:
      case Func::kGetDistance:
      case Func::kGetIsId:
        c.param1 = remap(c.param1);  // a form id (quest / item / target / base)
        break;
      default:
        break;  // kGetActorValue param1 is an AV index; kRaw is unknown
    }
    if (c.run_on == RunOn::kReference) c.reference = remap(c.reference);
  }
}

ConditionList ParseConditions(const bethesda::Record& record) {
  constexpr u32 kCtda = FourCc('C', 'T', 'D', 'A');
  ConditionList list;
  for (const bethesda::Subrecord& sub : record.subrecords) {
    if (sub.type != kCtda) continue;
    Comparison c;
    if (ParseCtda(sub.data, &c)) list.comparisons.push_back(c);
  }
  return list;
}

}  // namespace rec::quest

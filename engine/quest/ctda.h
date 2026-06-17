#ifndef RECREATION_QUEST_CTDA_H_
#define RECREATION_QUEST_CTDA_H_

#include "core/types.h"
#include "quest/condition.h"

namespace rec::bethesda {
struct Record;
class RecordStore;
}  // namespace rec::bethesda

namespace rec::quest {

// Transpiles one CTDA subrecord payload into a native Comparison. Returns false
// if the payload is too short to be a CTDA. Handles the 28-byte classic layout
// and the 32-byte Skyrim SE layout (the trailing 4 SE bytes are ignored). The
// form-id params it produces are raw and plugin-relative; resolve them with
// ResolveConditionForms before evaluating against engine state.
bool ParseCtda(ByteSpan data, Comparison* out);

// Remaps the raw, plugin-relative form-id fields in `conditions` to packed
// GlobalFormId handles via the load order, using `plugin` (the index of the
// plugin the conditions were read from) as the resolution base. Only fields
// that are form ids for the comparison's function are touched; AV indices and
// literal params are left alone.
void ResolveConditionForms(ConditionList& conditions, const bethesda::RecordStore& records,
                           u16 plugin);

// Collects every CTDA subrecord in `record`, in order, into a ConditionList.
// Correct for records whose conditions are record-scoped (e.g. a dialogue INFO).
// QUST conditions are scoped to the preceding QSDT/QOBJ instead, so the quest
// importer parses CTDA per stage/objective rather than calling this.
ConditionList ParseConditions(const bethesda::Record& record);

}  // namespace rec::quest

#endif  // RECREATION_QUEST_CTDA_H_

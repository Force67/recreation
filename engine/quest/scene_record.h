#ifndef RECREATION_QUEST_SCENE_RECORD_H_
#define RECREATION_QUEST_SCENE_RECORD_H_

#include <vector>

#include "core/types.h"

namespace rec::bethesda {
struct Record;
class RecordStore;
}  // namespace rec::bethesda

namespace rec::quest {

// Engine-agnostic decode of a SCEN (Scene) record: the data the runtime needs
// to drive Skyrim's scripted scenes (the Helgen intro and escort are SCENs).
// This is the data layer only; the runtime maps a SceneDef onto live actors,
// dialogue and packages.
//
// SCEN layout (Skyrim SE), in declaration order:
//   EDID, VMAD, FNAM (scene flags u32), PNAM (owning quest form id),
//   a run of PHASES, a run of ACTORS, a run of ACTIONS, INAM, VNAM.
//
// A phase is delimited by HNAM markers and carries:
//   HNAM, NAM0 (name flag), [begin CTDA*], NEXT, [completion CTDA*], NEXT, WNAM
// An actor entry is ALID (alias index), LNAM (flags), DNAM (flags).
// An action is opened by ANAM (type u16) and closed by an empty ANAM, carrying
// the actor alias (ALID), the phase window (SNAM start, ENAM end) and a
// type-specific payload (see SceneActionDef).

// One scene action: a single beat within a phase performed by an actor alias.
struct SceneActionDef {
  enum class Kind {
    kDialogue,    // actor speaks a DIAL topic; `topic` is the DIAL form id
    kPackage,     // actor runs an AI package; `package` is the PACK form id
    kTimer,       // a wait/timer beat, no ref
    kUnknown,     // an action type this parser does not model
  };

  Kind kind = Kind::kUnknown;
  u16 raw_type = 0;       // ANAM action type as stored (0 dialogue, 1 package, 2 timer)
  i32 actor_alias = -1;   // ALID, scene alias index of the performer
  i32 start_phase = 0;    // SNAM, phase this action begins in
  i32 end_phase = 0;      // ENAM, phase this action ends in
  u32 flags = 0;          // FNAM action flags, 0 when absent

  u64 topic = 0;          // kDialogue: DIAL topic handle (packed GlobalFormId)
  u64 package = 0;        // kPackage: PACK handle (packed GlobalFormId)
  f32 timer_seconds = 0.0f;  // kTimer: wait duration (the SNAM after ENAM)

  // Dialogue-only delivery hints.
  i32 head_track_alias = -1;  // HTID, alias to look at while speaking, -1 none
  f32 delay_min = 0.0f;       // DMIN seconds
  f32 delay_max = 0.0f;       // DMAX seconds
  u32 emotion_type = 0;       // DEMO
  i32 emotion_value = 0;      // DEVA
};

// One scene phase: an ordered segment gated by begin/completion conditions.
// CTDA payloads are kept raw because this worktree has no ctda.h; the runtime
// can transpile them with the quest condition parser when it integrates.
struct SceneRawCondition {
  std::vector<u8> ctda;  // one CTDA subrecord payload, plugin-relative form ids
};

struct ScenePhaseDef {
  i32 index = 0;
  std::vector<SceneRawCondition> begin;       // gate to enter the phase
  std::vector<SceneRawCondition> completion;  // gate to advance past it
};

// One scene actor slot: a quest alias the scene animates.
struct SceneActorDef {
  i32 alias = -1;   // ALID, scene alias index
  u32 flags = 0;    // LNAM behaviour flags
  u32 flags2 = 0;   // DNAM behaviour flags
};

// A whole scene.
struct SceneDef {
  u64 handle = 0;   // packed GlobalFormId of the SCEN record
  u64 quest = 0;    // PNAM owning quest handle (packed GlobalFormId), 0 if none
  u32 flags = 0;    // FNAM scene flags
  std::vector<SceneActorDef> actors;
  std::vector<ScenePhaseDef> phases;
  std::vector<SceneActionDef> actions;
};

// Parses one already-decoded SCEN record into a SceneDef. `handle` is the
// scene's packed form id. `records` resolves the plugin-relative form ids the
// scene carries (owning quest, dialogue topics, packages) against the load
// order; pass null to leave those handles as raw form ids. Always succeeds; a
// sparse scene yields empty actor/phase/action lists.
SceneDef ParseSceneRecord(u64 handle, const bethesda::Record& record,
                          const bethesda::RecordStore* records);

}  // namespace rec::quest

#endif  // RECREATION_QUEST_SCENE_RECORD_H_

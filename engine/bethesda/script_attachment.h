#ifndef RECREATION_BETHESDA_SCRIPT_ATTACHMENT_H_
#define RECREATION_BETHESDA_SCRIPT_ATTACHMENT_H_

#include <string>
#include <vector>

#include "core/types.h"

namespace rec::bethesda {

// Parsed VMAD subrecord: the Papyrus scripts attached to a form plus the
// property values the editor baked in. This is the bridge from plugin data to
// the VM: the engine reads it to know which compiled scripts to instantiate on
// a form and how to seed their properties. Little-endian, like all plugin data.

// A scripted reference to another form, resolved later against the load order.
struct ScriptObjectValue {
  u32 form_id = 0;   // raw form id (plugin-local high byte), 0 = none
  u16 alias_id = 0;  // quest alias index, 0xffff = none
};

struct ScriptProperty {
  std::string name;
  u8 type = 0;     // 1 object, 2 string, 3 int, 4 float, 5 bool, 11-15 arrays
  u8 status = 0;

  i32 int_value = 0;
  f32 float_value = 0;
  bool bool_value = false;
  std::string string_value;
  ScriptObjectValue object_value;

  std::vector<i32> int_array;
  std::vector<f32> float_array;
  std::vector<u8> bool_array;
  std::vector<std::string> string_array;
  std::vector<ScriptObjectValue> object_array;
};

struct ScriptEntry {
  std::string name;  // compiled script object name (scripts/<name>.pex)
  u8 status = 0;
  std::vector<ScriptProperty> properties;
};

struct ScriptAttachment {
  i16 version = 0;
  i16 object_format = 0;
  std::vector<ScriptEntry> scripts;
};

// One quest stage's Papyrus fragment: the function the engine runs when the
// quest reaches `stage`. It lives on `script_name` (the auto-generated QF_<quest>
// script) as `function` (e.g. "Fragment_3").
struct QuestStageFragment {
  u16 stage = 0;
  std::string script_name;
  std::string function;
};

// Parses a VMAD subrecord body. Only the script list is read (the per-record
// fragment data that can follow it is record-type specific and skipped).
// Returns false on a malformed or truncated body.
bool ParseScriptAttachment(ByteSpan vmad, ScriptAttachment* out);

// Parses a QUST VMAD: the script list (into `out`) plus the quest fragment
// section that maps each stage to the Papyrus function run when it is set
// (into `fragments`). Alias fragments are skipped. Returns false if the script
// section is malformed; a malformed fragment tail leaves the fragments parsed
// so far.
bool ParseQuestFragments(ByteSpan vmad, ScriptAttachment* out,
                         std::vector<QuestStageFragment>* fragments);

// A dialogue response's Papyrus fragments: the function run when the line
// starts (begin) and ends (end). They live on the auto-generated TIF_<info>
// script. An empty `script_name` means the response has no fragment of that
// kind.
struct InfoFragment {
  std::string script_name;
  std::string function;
};
struct InfoFragments {
  InfoFragment begin;
  InfoFragment end;
};

// Parses an INFO VMAD: the script list (into `out`) plus the begin/end response
// fragments (into `frags`). Returns false if the script section is malformed; a
// malformed fragment tail leaves whatever parsed cleanly.
bool ParseInfoFragments(ByteSpan vmad, ScriptAttachment* out, InfoFragments* frags);

// A scene's Papyrus fragments. A SCEN's auto-generated SF_<scene> script runs
// begin/end for the whole scene and one fragment per phase boundary; the phase
// fragments are what call Quest.SetStage to advance the journal as the scene
// plays. An empty `script_name` means the scene has no fragment of that kind.
struct SceneFragment {
  std::string script_name;
  std::string function;
};
struct ScenePhaseFragment {
  u32 phase = 0;          // phase number this fragment runs at
  bool on_begin = true;   // true: run when the phase begins; false: when it ends
  SceneFragment fragment;
};
struct SceneFragments {
  SceneFragment begin;                     // run when the scene starts
  SceneFragment end;                       // run when the scene ends
  std::vector<ScenePhaseFragment> phases;  // per-phase begin fragments
};

// Parses a SCEN VMAD: the script list (into `out`) plus the scene begin/end and
// per-phase fragments (into `frags`). Returns false if the script section is
// malformed; a malformed fragment tail leaves whatever parsed cleanly.
bool ParseSceneFragments(ByteSpan vmad, ScriptAttachment* out, SceneFragments* frags);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_SCRIPT_ATTACHMENT_H_

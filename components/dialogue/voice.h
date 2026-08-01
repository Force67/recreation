#ifndef RECREATION_DIALOGUE_VOICE_H_
#define RECREATION_DIALOGUE_VOICE_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "core/types.h"

namespace rx::asset {
class Vfs;
}

namespace rx::bethesda {
class RecordStore;
}

namespace rx::dialogue {

// Where a spoken line's audio lives. Bethesda names voice assets off the records
// that own them, so a line's clip is addressable without any index:
//
//   sound/voice/<plugin>/<voice type>/<quest>_<topic>_<info id>_<response>.fuz
//
// e.g. sound/voice/skyrim.esm/malenord/mq101__0003374b_1.fuz (the topic segment is
// empty when the DIAL has no editor id, which is the norm for scene dialogue).
// The clip is what gives a cutscene its real pacing: the line lasts exactly as
// long as the recording, which is how the game times its scene phases.

// Builds the voice path for one response. `topic_edid` may be empty.
base::String VoiceFilePath(const base::String& plugin_file,
                           const base::String& voice_type,
                           const base::String& quest_edid,
                           const base::String& topic_edid,
                           u32 info_local_id,
                           int response_index);

// Every path worth probing for a line, best first: the plugin/quest/topic naming
// above, then the same file under the plugin's other spellings (a master's lines
// can be voiced in an update plugin) and with the response index dropped. The
// caller picks the first that exists in the Vfs.
base::Vector<base::String> VoiceFileCandidates(const base::Vector<base::String>& plugin_files,
                                               const base::String& voice_type,
                                               const base::String& quest_edid,
                                               const base::String& topic_edid,
                                               u32 info_local_id,
                                               int response_index);

// The VTCK voice type editor id of an NPC_ (empty when it has none). This is the
// directory the NPC's lines live in, so it is what turns an INFO into a file.
base::String VoiceTypeEditorId(const bethesda::RecordStore& records, bethesda::GlobalFormId npc);

// How long a line takes to read when there is no clip to measure: a speaking-rate
// estimate, clamped so a one-word line still registers and a long one does not
// stall a scene.
f32 EstimateLineSeconds(const base::String& text);

// An index of the voice archive, keyed by the INFO the recording belongs to.
//
// Guessing a clip's name from the records only works when the quest and topic
// segments match what the audio was exported against, and often they do not: a
// scene borrows another quest's topics, a line is voiced under a shared dialogue
// quest, an exporter dropped the topic. The one part of the name that is always
// the truth is the INFO's form id, so the index reads the archive once and looks a
// line up by (INFO, voice type). Everything after that is exact.
class VoiceIndex {
 public:
  // Enumerates `sound/voice/...` through the Vfs. Cheap (string work only) and
  // idempotent: the second call does nothing.
  void Build(const asset::Vfs& vfs);
  bool built() const { return built_; }
  size_t size() const { return by_info_.size(); }

  // The clip for a line, preferring the speaker's voice type and otherwise any
  // recording of that INFO. Empty when the archive has none.
  base::String Find(u32 info_local_id, const base::String& voice_type) const;

  // Parses one voice path into the INFO id and voice type it belongs to. False when
  // the name does not carry an id (a lip file, an unrelated asset).
  static bool ParsePath(base::StringRef path, u32* info_local_id, base::String* voice_type);

 private:
  struct Clip {
    base::String voice_type;
    base::String path;
  };
  base::UnorderedMap<u32, base::Vector<Clip>> by_info_;
  bool built_ = false;
};

// Playing time of a voice asset, read out of its header rather than by decoding it.
// A .fuz is a lipsync block in front of an xWMA RIFF, and xWMA carries a `dpds`
// table whose last entry is the total decoded byte count, so the exact length is a
// dozen reads away; plain WAV falls back to the data chunk over the byte rate.
// 0 when the bytes are not a container this understands. Cheap enough to call while
// a scene is playing, which is the point: a decode per line would hitch the frame.
f32 ClipSeconds(ByteSpan bytes);

}  // namespace rx::dialogue

#endif  // RECREATION_DIALOGUE_VOICE_H_

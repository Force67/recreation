#ifndef RECREATION_DIALOGUE_VOICE_H_
#define RECREATION_DIALOGUE_VOICE_H_

#include <string>
#include <vector>

#include "bethesda/form_id.h"
#include "core/types.h"

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
std::string VoiceFilePath(const std::string& plugin_file, const std::string& voice_type,
                          const std::string& quest_edid, const std::string& topic_edid,
                          u32 info_local_id, int response_index);

// Every path worth probing for a line, best first: the plugin/quest/topic naming
// above, then the same file under the plugin's other spellings (a master's lines
// can be voiced in an update plugin) and with the response index dropped. The
// caller picks the first that exists in the Vfs.
std::vector<std::string> VoiceFileCandidates(const std::vector<std::string>& plugin_files,
                                             const std::string& voice_type,
                                             const std::string& quest_edid,
                                             const std::string& topic_edid, u32 info_local_id,
                                             int response_index);

// The VTCK voice type editor id of an NPC_ (empty when it has none). This is the
// directory the NPC's lines live in, so it is what turns an INFO into a file.
std::string VoiceTypeEditorId(const bethesda::RecordStore& records, bethesda::GlobalFormId npc);

// How long a line takes to read when there is no clip to measure: a speaking-rate
// estimate, clamped so a one-word line still registers and a long one does not
// stall a scene.
f32 EstimateLineSeconds(const std::string& text);

// Playing time of a voice asset, read out of its header rather than by decoding it.
// A .fuz is a lipsync block in front of an xWMA RIFF, and xWMA carries a `dpds`
// table whose last entry is the total decoded byte count, so the exact length is a
// dozen reads away; plain WAV falls back to the data chunk over the byte rate.
// 0 when the bytes are not a container this understands. Cheap enough to call while
// a scene is playing, which is the point: a decode per line would hitch the frame.
f32 ClipSeconds(ByteSpan bytes);

}  // namespace rx::dialogue

#endif  // RECREATION_DIALOGUE_VOICE_H_

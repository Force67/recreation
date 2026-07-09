#ifndef RECREATION_DIALOGUE_DIALOGUE_H_
#define RECREATION_DIALOGUE_DIALOGUE_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "bethesda/form_id.h"
#include "core/types.h"
#include "quest/condition.h"

namespace rx::bethesda {
class RecordStore;
class StringTable;
struct Record;
}  // namespace rx::bethesda

namespace rx::dialogue {

// A dialogue form is addressed by its packed GlobalFormId, the same handle the
// quest system and Papyrus guest use.
using Handle = u64;

// One INFO: the player's line, the NPC's reply, and the Papyrus fragment that
// runs when the line plays (this is what advances a quest, e.g. via SetStage).
struct Response {
  Handle info = 0;
  std::string player_line;  // RNAM prompt, falls back to the topic text
  std::string npc_line;     // first NAM1 response text
  std::string fragment_script;    // TIF_<info> script, empty if none
  std::string fragment_function;  // begin fragment function, e.g. "Fragment_0"
  // Native condition list parsed from the INFO's CTDA. The response is only
  // available when these pass. ParseInfoRecord leaves the form-id params raw;
  // ParseTopic resolves them so they can be evaluated against engine state.
  quest::ConditionList conditions;
};

// One DIAL topic and the responses under it.
struct Topic {
  Handle dial = 0;
  std::string editor_id;
  std::string text;    // FULL topic prompt
  Handle quest = 0;    // QNAM quest handle, 0 if the topic is not quest-bound
  i32 priority = 0;
  std::vector<Response> responses;
};

// Parses one already-decoded INFO record into a Response. `topic_text` is the
// fallback player line when the INFO has no RNAM prompt. Pure, so it is unit
// testable without a record store.
Response ParseInfoRecord(const bethesda::Record& record, Handle info,
                         const std::string& topic_text, const bethesda::StringTable* strings);

// Parses one DIAL topic and its INFO children. `strings` resolves localized
// text (may be null). Returns a topic with dial == 0 if `dial` is not a DIAL.
Topic ParseTopic(const bethesda::RecordStore& records, bethesda::GlobalFormId dial,
                 const bethesda::StringTable* strings);

// True if `response` may be shown given the world state `ctx` exposes -- i.e.
// its parsed conditions pass. A response with no conditions is always available.
bool ResponseAvailable(const Response& response, const quest::ConditionContext& ctx);

// Flattens the responses of `topics` to those currently available under `ctx`,
// in topic then response order -- the player's dialogue menu for an NPC.
std::vector<Response> AvailableResponses(const std::vector<Topic>& topics,
                                         const quest::ConditionContext& ctx);

// A startup index from quest handle to the DIAL topics bound to it (by QNAM),
// so opening dialogue for a quest does not rescan every topic.
class DialogueDb {
 public:
  // Scans DIAL records once and buckets them by their QNAM quest.
  void Build(const bethesda::RecordStore& records);

  // DIAL handles bound to `quest`, empty if none.
  const std::vector<Handle>& TopicsForQuest(Handle quest) const;

  size_t topic_count() const { return topic_count_; }

 private:
  std::unordered_map<Handle, std::vector<Handle>> by_quest_;
  std::vector<Handle> empty_;
  size_t topic_count_ = 0;
};

}  // namespace rx::dialogue

#endif  // RECREATION_DIALOGUE_DIALOGUE_H_

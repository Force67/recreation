#ifndef RECREATION_DIALOGUE_DIALOGUE_H_
#define RECREATION_DIALOGUE_DIALOGUE_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>
#include <string>

#include "components/bethesda/form_id.h"
#include "components/quest/condition.h"
#include "core/types.h"

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
  base::String player_line;        // RNAM prompt, falls back to the topic text
  base::String npc_line;           // first NAM1 response text
  base::String fragment_script;    // TIF_<info> script, empty if none
  base::String fragment_function;  // begin fragment function, e.g. "Fragment_0"
  // Native condition list parsed from the INFO's CTDA. The response is only
  // available when these pass. ParseInfoRecord leaves the form-id params raw;
  // ParseTopic resolves them so they can be evaluated against engine state.
  quest::ConditionList conditions;
  // ENAM bit 0x4: the line is offered once and never again. Read off the record
  // rather than guessed: of the 9822 lines a 100% Skyrim save records as spoken,
  // the 399 carrying this bit hang off the one-shot topics (every ...IntroTopic,
  // ...InitialBranchTopic, ...ForcegreetTopic, CWVictoryTopic), and the
  // repeatable ones (carriage destinations, services) do not carry it.
  bool say_once = false;
};

// One DIAL topic and the responses under it.
struct Topic {
  Handle dial = 0;
  base::String editor_id;
  base::String text;  // FULL topic prompt
  Handle quest = 0;   // QNAM quest handle, 0 if the topic is not quest-bound
  i32 priority = 0;
  base::Vector<Response> responses;
};

// Parses one already-decoded INFO record into a Response. `topic_text` is the
// fallback player line when the INFO has no RNAM prompt. Pure, so it is unit
// testable without a record store.
// `plugin` is the load-order index the record came from: string ids are only
// unique within a plugin, so a DLC line needs its own table or it reads back as
// whatever the base game wrote at that id.
Response ParseInfoRecord(const bethesda::Record& record,
                         Handle info,
                         const base::String& topic_text,
                         const bethesda::StringTable* strings,
                         u16 plugin = 0xffff);

// Parses one DIAL topic and its INFO children. `strings` resolves localized
// text (may be null). Returns a topic with dial == 0 if `dial` is not a DIAL.
Topic ParseTopic(const bethesda::RecordStore& records,
                 bethesda::GlobalFormId dial,
                 const bethesda::StringTable* strings);

// True if `response` may be shown given the world state `ctx` exposes -- i.e.
// its parsed conditions pass. A response with no conditions is always available.
bool ResponseAvailable(const Response& response, const quest::ConditionContext& ctx);

// Flattens the responses of `topics` to those currently available under `ctx`,
// in topic then response order -- the player's dialogue menu for an NPC.
base::Vector<Response> AvailableResponses(const base::Vector<Topic>& topics,
                                          const quest::ConditionContext& ctx);

// The lines the player has already heard, one entry per INFO. Kept per response
// and not per topic because that is how the games record it: a topic stays open
// while any line under it is still unheard, and only the say-once ones close.
//
// Filled from a resumed savegame and added to as the player talks, so a line
// exhausted three hundred hours ago and one exhausted a minute ago read alike.
class SaidTopics {
 public:
  void MarkSaid(Handle info) { said_[info] = true; }
  bool Said(Handle info) const { return said_.find(info) != nullptr; }
  size_t size() const { return said_.size(); }
  void Clear() { said_.clear(); }

 private:
  base::UnorderedMap<Handle, bool> said_;
};

// A startup index from quest handle to the DIAL topics bound to it (by QNAM),
// so opening dialogue for a quest does not rescan every topic.
class DialogueDb {
 public:
  // Scans DIAL records once and buckets them by their QNAM quest.
  void Build(const bethesda::RecordStore& records);

  // DIAL handles bound to `quest`, empty if none.
  const base::Vector<Handle>& TopicsForQuest(Handle quest) const;

  size_t topic_count() const { return topic_count_; }

 private:
  base::UnorderedMap<Handle, base::Vector<Handle>> by_quest_;
  base::Vector<Handle> empty_;
  size_t topic_count_ = 0;
};

}  // namespace rx::dialogue

#endif  // RECREATION_DIALOGUE_DIALOGUE_H_

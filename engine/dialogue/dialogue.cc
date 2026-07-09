#include "dialogue/dialogue.h"

#include <cstring>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "bethesda/script_attachment.h"
#include "bethesda/strings.h"
#include "core/types.h"
#include "quest/ctda.h"

namespace rx::dialogue {
namespace {

// A localized subrecord: a 4-byte string id in a localized plugin, inline
// zero-terminated text otherwise. Mirrors the quest/binding text handling.
std::string ResolveLString(const bethesda::Subrecord& sub, const bethesda::StringTable* strings) {
  if (strings && sub.data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, sub.data.data(), 4);
    if (const base::String* s = strings->Find(string_id)) return std::string(s->c_str());
  }
  const char* p = reinterpret_cast<const char*>(sub.data.data());
  size_t n = sub.data.size();
  size_t len = 0;
  while (len < n && p[len] != '\0') ++len;
  return std::string(p, len);
}

}  // namespace

Response ParseInfoRecord(const bethesda::Record& record, Handle info,
                         const std::string& topic_text, const bethesda::StringTable* strings) {
  Response out;
  out.info = info;
  if (const bethesda::Subrecord* rnam = record.Find(FourCc('R', 'N', 'A', 'M')))
    out.player_line = ResolveLString(*rnam, strings);
  if (out.player_line.empty()) out.player_line = topic_text;
  // The response text is the first NAM1 (one per response row, after its TRDT).
  if (const bethesda::Subrecord* nam1 = record.Find(FourCc('N', 'A', 'M', '1')))
    out.npc_line = ResolveLString(*nam1, strings);

  if (const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'))) {
    bethesda::ScriptAttachment attachment;
    bethesda::InfoFragments frags;
    if (bethesda::ParseInfoFragments(vmad->data, &attachment, &frags)) {
      out.fragment_script = frags.begin.script_name;
      out.fragment_function = frags.begin.function;
    }
  }

  // CTDA conditions gate whether this line is available. Left raw here (form-id
  // params unresolved); ParseTopic resolves them against the load order.
  out.conditions = quest::ParseConditions(record);
  return out;
}

bool ResponseAvailable(const Response& response, const quest::ConditionContext& ctx) {
  return quest::Evaluate(response.conditions, ctx);
}

std::vector<Response> AvailableResponses(const std::vector<Topic>& topics,
                                         const quest::ConditionContext& ctx) {
  std::vector<Response> out;
  for (const Topic& topic : topics)
    for (const Response& response : topic.responses)
      if (ResponseAvailable(response, ctx)) out.push_back(response);
  return out;
}

Topic ParseTopic(const bethesda::RecordStore& records, bethesda::GlobalFormId dial,
                 const bethesda::StringTable* strings) {
  Topic out;
  const bethesda::RecordStore::StoredRecord* stored = records.Find(dial);
  if (!stored || stored->header.type != FourCc('D', 'I', 'A', 'L')) return out;
  bethesda::Record record;
  if (!records.Parse(dial, &record)) return out;

  out.dial = dial.packed();
  out.editor_id = record.GetString(FourCc('E', 'D', 'I', 'D'));
  if (const bethesda::Subrecord* full = record.Find(FourCc('F', 'U', 'L', 'L')))
    out.text = ResolveLString(*full, strings);
  if (const bethesda::Subrecord* pnam = record.Find(FourCc('P', 'N', 'A', 'M'));
      pnam && pnam->data.size() >= 4) {
    f32 priority;
    std::memcpy(&priority, pnam->data.data(), 4);
    out.priority = static_cast<i32>(priority);
  }
  if (const bethesda::Subrecord* qnam = record.Find(FourCc('Q', 'N', 'A', 'M'));
      qnam && qnam->data.size() >= 4) {
    u32 raw;
    std::memcpy(&raw, qnam->data.data(), 4);
    out.quest = records.ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed();
  }

  if (const base::Vector<u64>* infos = records.TopicInfos(dial)) {
    for (u64 packed : *infos) {
      bethesda::GlobalFormId info{static_cast<u16>(packed >> 32),
                                  static_cast<u32>(packed & 0xffffffffu)};
      bethesda::Record info_record;
      if (!records.Parse(info, &info_record)) continue;
      Response response = ParseInfoRecord(info_record, packed, out.text, strings);
      // Resolve the condition form ids against the INFO's own plugin, so a
      // GetStage(quest) condition names the same packed handle the quest system
      // uses.
      const bethesda::RecordStore::StoredRecord* info_stored = records.Find(info);
      quest::ResolveConditionForms(response.conditions, records,
                                   info_stored ? info_stored->winning_plugin : info.plugin);
      out.responses.push_back(std::move(response));
    }
  }
  return out;
}

void DialogueDb::Build(const bethesda::RecordStore& records) {
  records.EachOfType(FourCc('D', 'I', 'A', 'L'),
                     [&](bethesda::GlobalFormId id,
                         const bethesda::RecordStore::StoredRecord& stored) {
                       ++topic_count_;
                       bethesda::Record record;
                       if (!records.Parse(id, &record)) return;
                       const bethesda::Subrecord* qnam = record.Find(FourCc('Q', 'N', 'A', 'M'));
                       if (!qnam || qnam->data.size() < 4) return;
                       u32 raw;
                       std::memcpy(&raw, qnam->data.data(), 4);
                       u64 quest = records.ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin).packed();
                       if (quest != 0) by_quest_[quest].push_back(id.packed());
                     });
}

const std::vector<Handle>& DialogueDb::TopicsForQuest(Handle quest) const {
  auto it = by_quest_.find(quest);
  return it == by_quest_.end() ? empty_ : it->second;
}

}  // namespace rx::dialogue

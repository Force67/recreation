#include "quest/quest_def.h"

#include <cstring>

#include "bethesda/record.h"
#include "bethesda/strings.h"
#include "core/types.h"

namespace rec::quest {
namespace {

// Resolves a localized string subrecord: a 4-byte string id in a localized
// plugin, inline zero-terminated text otherwise. Mirrors the binding's FULL
// handling so quest text reads identically.
std::string ResolveLString(const bethesda::Subrecord& sub, const bethesda::StringTable* strings) {
  if (strings && sub.data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, sub.data.data(), 4);
    if (const base::String* s = strings->Find(string_id)) return std::string(s->c_str());
  }
  // Inline text: bytes up to the terminator.
  const char* p = reinterpret_cast<const char*>(sub.data.data());
  size_t n = sub.data.size();
  size_t len = 0;
  while (len < n && p[len] != '\0') ++len;
  return std::string(p, len);
}

template <typename T>
T ReadLe(const bethesda::Subrecord& sub) {
  T value{};
  if (sub.data.size() >= sizeof(T)) std::memcpy(&value, sub.data.data(), sizeof(T));
  return value;
}

}  // namespace

const StageDef* QuestDef::FindStage(i32 index) const {
  const StageDef* best = nullptr;
  for (const StageDef& s : stages) {
    if (s.index != index) continue;
    // Last log entry for the index wins, matching the journal showing the most
    // recent entry the script set.
    best = &s;
  }
  return best;
}

const ObjectiveDef* QuestDef::FindObjective(i32 index) const {
  for (const ObjectiveDef& o : objectives)
    if (o.index == index) return &o;
  return nullptr;
}

const AliasDef* QuestDef::FindAlias(i32 id) const {
  for (const AliasDef& a : aliases)
    if (a.id == id) return &a;
  return nullptr;
}

i32 QuestDef::CompletionStage() const {
  i32 best = -1;
  for (const StageDef& s : stages) {
    if (!s.complete_quest) continue;
    if (best < 0 || s.index < best) best = s.index;
  }
  return best;
}

QuestDef ParseQuestDefinition(u64 handle, const bethesda::Record& record,
                              const bethesda::StringTable* strings) {
  constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
  constexpr u32 kFull = FourCc('F', 'U', 'L', 'L');
  constexpr u32 kDnam = FourCc('D', 'N', 'A', 'M');
  constexpr u32 kIndx = FourCc('I', 'N', 'D', 'X');
  constexpr u32 kQsdt = FourCc('Q', 'S', 'D', 'T');
  constexpr u32 kCnam = FourCc('C', 'N', 'A', 'M');
  constexpr u32 kQobj = FourCc('Q', 'O', 'B', 'J');
  constexpr u32 kNnam = FourCc('N', 'N', 'A', 'M');
  constexpr u32 kQsta = FourCc('Q', 'S', 'T', 'A');
  constexpr u32 kAlst = FourCc('A', 'L', 'S', 'T');  // reference alias id
  constexpr u32 kAlls = FourCc('A', 'L', 'L', 'S');  // location alias id
  constexpr u32 kAlid = FourCc('A', 'L', 'I', 'D');  // alias name (e.g. "City")
  constexpr u32 kAlfr = FourCc('A', 'L', 'F', 'R');  // forced reference form id
  constexpr u32 kAlua = FourCc('A', 'L', 'U', 'A');  // unique-actor NPC_ base
  constexpr u32 kAled = FourCc('A', 'L', 'E', 'D');  // alias block end

  QuestDef def;
  def.handle = handle;

  i32 cur_stage = 0;
  bool in_stage = false;      // an INDX has opened a stage block
  bool in_objective = false;  // a QOBJ has opened an objective block
  bool in_alias = false;      // an ALST/ALLS has opened an alias block

  // Subrecords appear in declaration order: a stage is INDX then one or more
  // QSDT (+CNAM); an objective is QOBJ, NNAM, then its QSTA targets.
  for (const bethesda::Subrecord& sub : record.subrecords) {
    switch (sub.type) {
      case kEdid:
        def.editor_id = record.GetString(kEdid);
        break;
      case kFull:
        def.name = ResolveLString(sub, strings);
        break;
      case kDnam:
        // DNAM: flags(u16), priority(u8), ... Priority sits at byte offset 2.
        if (sub.data.size() >= 3) def.priority = sub.data.data()[2];
        break;
      case kIndx:
        cur_stage = ReadLe<u16>(sub);
        in_stage = true;
        in_objective = false;
        in_alias = false;
        break;
      case kQsdt: {
        if (!in_stage) break;
        StageDef stage;
        stage.index = cur_stage;
        if (sub.data.size() >= 1) stage.complete_quest = (sub.data.data()[0] & 0x01) != 0;
        def.stages.push_back(std::move(stage));
        break;
      }
      case kCnam:
        if (in_stage && !def.stages.empty()) def.stages.back().log_entry = ResolveLString(sub, strings);
        break;
      case kQobj: {
        ObjectiveDef obj;
        obj.index = static_cast<i16>(ReadLe<u16>(sub));
        def.objectives.push_back(std::move(obj));
        in_objective = true;
        in_stage = false;
        in_alias = false;
        break;
      }
      case kNnam:
        if (in_objective && !def.objectives.empty())
          def.objectives.back().text = ResolveLString(sub, strings);
        break;
      case kQsta:
        if (in_objective && !def.objectives.empty())
          def.objectives.back().target_aliases.push_back(ReadLe<i32>(sub));
        break;
      case kAlst:
      case kAlls: {
        // A new alias block. Reference (ALST) and location (ALLS) aliases share
        // the id space an objective's QSTA points into.
        AliasDef alias;
        alias.id = ReadLe<i32>(sub);
        def.aliases.push_back(alias);
        in_alias = true;
        in_stage = false;
        in_objective = false;
        break;
      }
      case kAlid:
        if (in_alias && !def.aliases.empty()) {
          const char* p = reinterpret_cast<const char*>(sub.data.data());
          size_t n = sub.data.size(), len = 0;
          while (len < n && p[len] != '\0') ++len;
          def.aliases.back().name.assign(p, len);
        }
        break;
      case kAlfr:
        if (in_alias && !def.aliases.empty())
          def.aliases.back().forced_ref_raw = ReadLe<u32>(sub);
        break;
      case kAlua:
        if (in_alias && !def.aliases.empty())
          def.aliases.back().unique_actor_raw = ReadLe<u32>(sub);
        break;
      case kAled:
        in_alias = false;
        break;
      default:
        break;
    }
  }
  return def;
}

}  // namespace rec::quest

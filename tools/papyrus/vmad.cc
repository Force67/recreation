// vmaddump: read Papyrus script attachments (VMAD) straight out of the game's
// plugins, validating the parser against shipped records.
//
//   vmaddump <data_dir> [RECORD_TYPE] [max]
//
// RECORD_TYPE is a 4-char record signature (default QUST). Lists, for the first
// `max` records of that type carrying scripts, the attached script names and
// their baked-in property values.

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "bethesda/script_attachment.h"

namespace {

using namespace rx;
using namespace rx::bethesda;

std::string PropertySummary(const ScriptProperty& p) {
  switch (p.type) {
    case 1:
      return std::string("object form=") + std::to_string(p.object_value.form_id);
    case 2:
      return "string \"" + p.string_value + "\"";
    case 3:
      return "int " + std::to_string(p.int_value);
    case 4:
      return "float " + std::to_string(p.float_value);
    case 5:
      return std::string("bool ") + (p.bool_value ? "true" : "false");
    case 11:
      return "object[" + std::to_string(p.object_array.size()) + "]";
    case 12:
      return "string[" + std::to_string(p.string_array.size()) + "]";
    case 13:
      return "int[" + std::to_string(p.int_array.size()) + "]";
    case 14:
      return "float[" + std::to_string(p.float_array.size()) + "]";
    case 15:
      return "bool[" + std::to_string(p.bool_array.size()) + "]";
    default:
      return "type " + std::to_string(p.type);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <data_dir> [RECORD_TYPE] [max]\n", argv[0]);
    return 2;
  }
  std::string data_dir = argv[1];
  char sig[4] = {'Q', 'U', 'S', 'T'};
  if (argc > 2)
    for (int i = 0; i < 4; ++i) sig[i] = argv[2][i] ? argv[2][i] : ' ';
  u32 type = FourCc(sig[0], sig[1], sig[2], sig[3]);
  int max = argc > 3 ? std::atoi(argv[3]) : 8;

  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) {
    std::printf("failed to load records from %s\n", data_dir.c_str());
    return 1;
  }
  std::printf("loaded %zu records\n", records.record_count());

  int total = 0;
  int shown = 0;
  records.EachOfType(type, [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    Record rec;
    if (!records.Parse(id, &rec)) return;
    const Subrecord* vmad = rec.Find(FourCc('V', 'M', 'A', 'D'));
    if (!vmad) return;
    ScriptAttachment att;
    std::vector<QuestStageFragment> fragments;
    bool is_quest = type == FourCc('Q', 'U', 'S', 'T');
    bool ok = is_quest ? ParseQuestFragments(vmad->data, &att, &fragments)
                       : ParseScriptAttachment(vmad->data, &att);
    if (!ok || att.scripts.empty()) return;
    ++total;
    if (shown >= max) return;
    ++shown;
    std::printf("%04x:%06x  vmad v%d fmt%d, %zu script(s)\n", id.plugin, id.local_id, att.version,
                att.object_format, att.scripts.size());
    for (const ScriptEntry& s : att.scripts) {
      std::printf("  script %s (%zu properties)\n", s.name.c_str(), s.properties.size());
      for (const ScriptProperty& p : s.properties)
        std::printf("    %-28s %s\n", p.name.c_str(), PropertySummary(p).c_str());
    }
    for (const QuestStageFragment& f : fragments)
      std::printf("  stage %-4u -> %s.%s\n", f.stage, f.script_name.c_str(), f.function.c_str());
  });

  char type_name[5] = {sig[0], sig[1], sig[2], sig[3], 0};
  std::printf("%d %s records carry scripts (showed %d)\n", total, type_name, shown);
  return 0;
}

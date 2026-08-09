// savegame_applytest: checks for applying a decoded save onto a live world.
//
// Three halves. The first drives FormRemap with synthetic load orders, which is
// where the correctness of everything else comes from: a save's form id is only
// meaningful against the order it was written under. The second synthesises
// change forms and asserts what reaches the sink. The third opens a real 100%
// complete Skyrim SE save and asserts the player placement and the apply tally
// read out of its bytes; it is skipped when the file is not on this machine.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "components/bethesda/savegame.h"
#include "components/bethesda/savegame_apply.h"
#include "components/bethesda/savegame_changeform.h"
#include "core/types.h"

namespace {

using rx::f32;
using rx::i32;
using rx::u16;
using rx::u32;
using rx::u8;
using rx::bethesda::ChangeForm;
using rx::bethesda::ChangeFormType;
using rx::bethesda::FormRemap;
using rx::bethesda::GlobalFormId;
using rx::bethesda::SaveApplyStats;
using rx::bethesda::SaveFile;
using rx::bethesda::SaveFormat;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

// A load order the game "has loaded", answering FormRemap::Build.
struct RuntimeOrder {
  base::Vector<base::String> plugins;

  u16 IndexOf(const base::String& name) const {
    for (size_t i = 0; i < plugins.size(); ++i) {
      if (plugins[i] == name)
        return static_cast<u16>(i);
    }
    return 0xffff;
  }
};

FormRemap BuildRemap(const SaveFile& save, const RuntimeOrder& order) {
  FormRemap remap;
  remap.Build(save, [&order](const base::String& name) { return order.IndexOf(name); });
  return remap;
}

// Records everything applied, so a test can assert the sink saw exactly what
// the save carries.
class RecordingSink : public rx::bethesda::SaveSink {
 public:
  struct QuestState {
    GlobalFormId quest;
    i32 stage = 0;
    bool running = false;
    bool complete = false;
  };
  struct Value {
    GlobalFormId form;
    base::String name;
    f32 value = 0;
  };

  void SetGlobal(GlobalFormId global, f32 value) override {
    globals.push_back({global, "", value});
  }
  void SetQuestStageDone(GlobalFormId quest, i32 stage) override {
    stages.push_back({quest, stage});
  }
  void SetQuestState(GlobalFormId quest, i32 stage, bool running, bool complete) override {
    quests.push_back({quest, stage, running, complete});
  }
  void SetQuestObjective(GlobalFormId quest,
                         i32 objective,
                         bool displayed,
                         bool completed) override {
    objectives.push_back({quest, objective, displayed, completed});
  }
  void FillQuestAlias(GlobalFormId quest, u32 alias_id, GlobalFormId ref) override {
    aliases.push_back({quest, alias_id, ref});
  }
  void SetActorValue(GlobalFormId actor_base, base::StringRef name, f32 value) override {
    actor_values.push_back({actor_base, base::String(name.data(), name.size()), value});
  }
  void SetActorFactionRank(GlobalFormId actor_base, GlobalFormId faction, i32 rank) override {
    faction_ranks.push_back({actor_base, faction, rank});
  }
  void SetFactionReaction(GlobalFormId faction, GlobalFormId other, i32 reaction) override {
    reactions.push_back({faction, other, reaction});
  }
  void SetActorLevel(GlobalFormId actor_base, u32 level) override {
    levels.push_back({actor_base, level});
  }
  void SetActorAi(GlobalFormId actor_base, const rx::bethesda::ActorAi& ai) override {
    ai_profiles.push_back({actor_base, ai});
  }
  void SetFactionInfamy(GlobalFormId faction, u32 violent, u32 non_violent) override {
    infamy.push_back({faction, violent, non_violent});
  }
  void SetDialogueSaid(GlobalFormId info) override { said.push_back(info); }
  void SetCellVisited(GlobalFormId cell,
                      base::Span<const rx::bethesda::CellVisitedGrid> grids) override {
    u32 tiles = 0;
    for (const rx::bethesda::CellVisitedGrid& grid : grids)
      for (u8 byte : grid.bits)
        tiles += static_cast<u32>(__builtin_popcount(byte));
    visited.push_back({cell, static_cast<u32>(grids.size()), tiles});
  }
  void SetReferenceEnabled(GlobalFormId ref, bool enabled) override {
    enabled_refs.push_back({ref, enabled});
  }
  void MoveReference(GlobalFormId ref,
                     GlobalFormId parent,
                     const f32 position[3],
                     const f32 rotation[3]) override {
    Move move;
    move.ref = ref;
    move.parent = parent;
    for (u32 i = 0; i < 3; ++i) {
      move.position[i] = position[i];
      move.rotation[i] = rotation[i];
    }
    moves.push_back(move);
  }

  struct StageDone {
    GlobalFormId quest;
    i32 stage;
  };
  struct Objective {
    GlobalFormId quest;
    i32 index;
    bool displayed;
    bool completed;
  };
  struct Alias {
    GlobalFormId quest;
    u32 alias_id;
    GlobalFormId ref;
  };
  struct Rank {
    GlobalFormId actor;
    GlobalFormId faction;
    i32 rank;
  };
  struct Enabled {
    GlobalFormId ref;
    bool enabled;
  };
  struct Move {
    GlobalFormId ref;
    GlobalFormId parent;
    f32 position[3] = {};
    f32 rotation[3] = {};
  };

  base::Vector<Value> globals;
  base::Vector<StageDone> stages;
  base::Vector<QuestState> quests;
  base::Vector<Objective> objectives;
  base::Vector<Alias> aliases;
  base::Vector<Value> actor_values;
  base::Vector<Rank> faction_ranks;
  base::Vector<Rank> reactions;
  struct Level {
    GlobalFormId actor;
    u32 level;
  };
  struct Ai {
    GlobalFormId actor;
    rx::bethesda::ActorAi ai;
  };
  struct Infamy {
    GlobalFormId faction;
    u32 violent;
    u32 non_violent;
  };
  struct Visited {
    GlobalFormId cell;
    u32 grids;
    u32 tiles;
  };

  base::Vector<Enabled> enabled_refs;
  base::Vector<Move> moves;
  base::Vector<Level> levels;
  base::Vector<Ai> ai_profiles;
  base::Vector<Infamy> infamy;
  base::Vector<GlobalFormId> said;
  base::Vector<Visited> visited;

  const Level* FindLevel(u32 local_id) const {
    for (const Level& entry : levels)
      if (entry.actor.plugin == 0 && entry.actor.local_id == local_id)
        return &entry;
    return nullptr;
  }
  const Ai* FindAi(u32 local_id) const {
    for (const Ai& entry : ai_profiles)
      if (entry.actor.plugin == 0 && entry.actor.local_id == local_id)
        return &entry;
    return nullptr;
  }
  const Infamy* FindInfamy(u32 local_id) const {
    for (const Infamy& entry : infamy)
      if (entry.faction.plugin == 0 && entry.faction.local_id == local_id)
        return &entry;
    return nullptr;
  }
  const Visited* FindVisited(u32 local_id) const {
    for (const Visited& entry : visited)
      if (entry.cell.plugin == 0 && entry.cell.local_id == local_id)
        return &entry;
    return nullptr;
  }
  bool Said(u32 local_id) const {
    for (const GlobalFormId& info : said)
      if (info.plugin == 0 && info.local_id == local_id)
        return true;
    return false;
  }
};

void PutU8(base::Vector<u8>& b, u8 v) {
  b.push_back(v);
}
void PutU16(base::Vector<u8>& b, u16 v) {
  b.push_back(u8(v));
  b.push_back(u8(v >> 8));
}
void PutU32(base::Vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i)
    b.push_back(u8(v >> (8 * i)));
}
void PutF32(base::Vector<u8>& b, f32 v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  PutU32(b, bits);
}
// Three big endian bytes, the two bit kind on top. Kind 1 resolves straight to
// a 0x00xxxxxx form id, which keeps the synthetic payloads readable.
void PutRef(base::Vector<u8>& b, u8 kind, u32 value) {
  b.push_back(u8((kind << 6) | ((value >> 16) & 0x3f)));
  b.push_back(u8(value >> 8));
  b.push_back(u8(value));
}
// The 1 byte form of the variable width count.
void PutCount(base::Vector<u8>& b, u8 count) {
  b.push_back(u8(count << 2));
}

void TestRemap() {
  std::puts("form remap");

  SaveFile save;
  save.plugins.push_back("Skyrim.esm");
  save.plugins.push_back("Dawnguard.esm");
  save.plugins.push_back("Mod.esp");
  save.light_plugins.push_back("Small.esl");

  // The running game loaded the same plugins in a different order, plus one the
  // save never saw. Nothing may resolve by index.
  RuntimeOrder order;
  order.plugins.push_back("Skyrim.esm");
  order.plugins.push_back("Update.esm");
  order.plugins.push_back("Dawnguard.esm");
  order.plugins.push_back("Small.esl");
  order.plugins.push_back("Mod.esp");

  const FormRemap remap = BuildRemap(save, order);
  Check("built", remap.built());
  Check("nothing missing", remap.missing_plugins().empty());

  GlobalFormId id;
  Check("first master keeps index 0",
        remap.Map(0x00000014, &id) && id.plugin == 0 && id.local_id == 0x14);
  Check("shifted master remaps",
        remap.Map(0x0100ABCD, &id) && id.plugin == 2 && id.local_id == 0x00ABCD);
  Check("plugin remaps past the inserted one",
        remap.Map(0x02001234, &id) && id.plugin == 4 && id.local_id == 0x001234);
  // A light plugin holds no mod index: its forms live at 0xFE<slot><local>.
  Check("light slot remaps", remap.Map(0xFE000801, &id) && id.plugin == 3 && id.local_id == 0x801);

  FormRemap::Refusal why = FormRemap::Refusal::kNone;
  Check("created form refused",
        !remap.Map(0xFF001234, &id, &why) && why == FormRemap::Refusal::kCreated);
  Check("mod index past the save's list refused",
        !remap.Map(0x03000001, &id, &why) && why == FormRemap::Refusal::kOutOfRange);
  Check("light slot past the save's list refused",
        !remap.Map(0xFE001801, &id, &why) && why == FormRemap::Refusal::kOutOfRange);

  // The same save against a game that never loaded the mod: every id from it is
  // refused by name, and the plugin is reported so a player can be told why.
  RuntimeOrder without;
  without.plugins.push_back("Skyrim.esm");
  without.plugins.push_back("Dawnguard.esm");
  const FormRemap partial = BuildRemap(save, without);
  Check("two plugins reported missing", partial.missing_plugins().size() == 2);
  Check("missing plugin named",
        !partial.missing_plugins().empty() && partial.missing_plugins()[0] == "Mod.esp");
  Check("id from a missing plugin refused",
        !partial.Map(0x02001234, &id, &why) && why == FormRemap::Refusal::kMissingPlugin);
  Check("id from a missing light plugin refused",
        !partial.Map(0xFE000801, &id, &why) && why == FormRemap::Refusal::kMissingPlugin);
  Check("ids from loaded plugins still map",
        partial.Map(0x0100ABCD, &id) && id.plugin == 1 && id.local_id == 0x00ABCD);

  // A save's mod index that the runtime happens to hold is still refused when
  // the name does not match, which is the whole point of remapping.
  RuntimeOrder swapped;
  swapped.plugins.push_back("Skyrim.esm");
  swapped.plugins.push_back("Other.esp");
  const FormRemap wrong = BuildRemap(save, swapped);
  Check("occupied but different plugin refused",
        !wrong.Map(0x01000001, &id, &why) && why == FormRemap::Refusal::kMissingPlugin);
}

void TestApplySynthetic() {
  std::puts("apply synthetic change forms");

  SaveFile save;
  save.format = SaveFormat::kSkyrimSe;
  save.plugins.push_back("Skyrim.esm");
  save.plugins.push_back("Mod.esp");
  save.form_ids.push_back(0x01000AAA);  // index 1: a ref living in the mod
  save.globals.push_back({0x00000035, 208.0f});
  save.globals.push_back({0x01000042, 7.0f});

  {
    ChangeForm quest;
    quest.form_id = 0x0003372B;
    quest.type = ChangeFormType::kQust;
    quest.version = 78;
    quest.flags = rx::bethesda::kQuestChangeStages | rx::bethesda::kQuestChangeObjectives |
                  rx::bethesda::kQuestChangeRunData;
    PutU8(quest.data, 0x22);  // completed, run once, not started
    PutU8(quest.data, 1);     // priority
    PutCount(quest.data, 3);
    PutU16(quest.data, 200);
    PutU8(quest.data, 1);  // run
    PutU16(quest.data, 10);
    PutU8(quest.data, 1);  // run, and lower than the one before it
    PutU16(quest.data, 50);
    PutU8(quest.data, 0);  // not run
    PutCount(quest.data, 1);
    PutU32(quest.data, 3);
    PutU32(quest.data, 0x3);   // displayed + completed
    PutU8(quest.data, 0);      // run data: unknown byte
    PutU32(quest.data, 1);     // one alias fill
    PutU32(quest.data, 5);     // alias id
    PutU8(quest.data, 0);      // unknown byte
    PutRef(quest.data, 0, 1);  // form id map index 1 -> 0x01000AAA
    save.change_forms.push_back(quest);
  }
  {
    ChangeForm actor;
    actor.form_id = 0x00013480;
    actor.type = ChangeFormType::kNpc;
    actor.version = 78;
    actor.flags = rx::bethesda::kActorBaseChangeFactions | rx::bethesda::kActorBaseChangeSkills;
    PutCount(actor.data, 1);
    PutRef(actor.data, 1, 0x0002BF9A);  // faction
    PutU8(actor.data, 3);               // rank
    for (u32 i = 0; i < rx::bethesda::kActorSkillCount; ++i)
      PutU8(actor.data, u8(15 + i));
    for (u32 i = 0; i < rx::bethesda::kActorSkillCount; ++i)
      PutU8(actor.data, 1);
    PutU16(actor.data, 500);  // health
    PutU16(actor.data, 300);  // magicka
    PutU16(actor.data, 400);  // stamina
    PutU16(actor.data, 0);
    PutF32(actor.data, 0.0f);
    PutU32(actor.data, 0);
    save.change_forms.push_back(actor);
  }
  {
    ChangeForm ref;
    ref.form_id = 0x01000AAA;
    ref.type = ChangeFormType::kRefr;
    ref.version = 78;
    ref.flags = rx::bethesda::kRefrChangeMoved | rx::bethesda::kRefrChangeFormFlags;
    PutRef(ref.data, 1, 0x0000003C);  // Tamriel
    PutF32(ref.data, 100.0f);
    PutF32(ref.data, 200.0f);
    PutF32(ref.data, 300.0f);
    PutF32(ref.data, 0.0f);
    PutF32(ref.data, 0.0f);
    PutF32(ref.data, 1.5f);
    PutU32(ref.data, rx::bethesda::kFormFlagInitiallyDisabled);
    PutU16(ref.data, 0);
    save.change_forms.push_back(ref);
  }
  {
    // The player: an ACHR, so the transform is followed by the eight bytes an
    // actor writes with no flag asking for them.
    ChangeForm player;
    player.form_id = 0x00000014;
    player.type = ChangeFormType::kAchr;
    player.version = 78;
    player.flags = rx::bethesda::kRefrChangeMoved;
    PutRef(player.data, 1, 0x0000003C);
    PutF32(player.data, -24067.2969f);
    PutF32(player.data, 97838.0469f);
    PutF32(player.data, -13344.8779f);
    PutF32(player.data, 0.0f);
    PutF32(player.data, 0.0f);
    PutF32(player.data, 0.5f);
    PutU32(player.data, 0xffffffff);
    PutU32(player.data, 0);
    save.change_forms.push_back(player);
  }

  RuntimeOrder order;
  order.plugins.push_back("Skyrim.esm");
  order.plugins.push_back("Update.esm");
  order.plugins.push_back("Mod.esp");
  const FormRemap remap = BuildRemap(save, order);

  RecordingSink sink;
  SaveApplyStats stats;
  rx::bethesda::ApplySave(save, remap, sink, &stats);

  Check("both globals applied", sink.globals.size() == 2);
  Check("global remapped", sink.globals.size() == 2 && sink.globals[1].form.plugin == 2 &&
                               sink.globals[1].form.local_id == 0x42);

  Check("one quest", sink.quests.size() == 1);
  Check("two stages run", sink.stages.size() == 2);
  Check("stages ascend",
        sink.stages.size() == 2 && sink.stages[0].stage == 10 && sink.stages[1].stage == 200);
  Check("current stage is the highest run", !sink.quests.empty() && sink.quests[0].stage == 200);
  Check("quest completed, not running",
        !sink.quests.empty() && sink.quests[0].complete && !sink.quests[0].running);
  Check("objective applied", sink.objectives.size() == 1 && sink.objectives[0].index == 3 &&
                                 sink.objectives[0].displayed && sink.objectives[0].completed);
  Check("alias filled through the form id map",
        sink.aliases.size() == 1 && sink.aliases[0].alias_id == 5 &&
            sink.aliases[0].ref.plugin == 2 && sink.aliases[0].ref.local_id == 0x000AAA);

  Check("skills and the three pools applied", sink.actor_values.size() == 21);
  Check("health first", !sink.actor_values.empty() && sink.actor_values[0].name == "Health" &&
                            sink.actor_values[0].value == 500.0f);
  Check("skill carries its offset", sink.actor_values.size() == 21 &&
                                        sink.actor_values[3].name == "OneHanded" &&
                                        sink.actor_values[3].value == 16.0f);
  Check("faction rank applied", sink.faction_ranks.size() == 1 && sink.faction_ranks[0].rank == 3);

  Check("reference moved", sink.moves.size() == 1);
  Check("moved into the remapped worldspace",
        sink.moves.size() == 1 && sink.moves[0].parent.plugin == 0 &&
            sink.moves[0].parent.local_id == 0x3C && sink.moves[0].position[1] == 200.0f);
  Check("reference disabled", sink.enabled_refs.size() == 1 && !sink.enabled_refs[0].enabled);

  rx::bethesda::PlayerPlacement placement;
  Check("player found", rx::bethesda::FindPlayerPlacement(save, remap, &placement));
  Check("player in the remapped worldspace",
        placement.valid && placement.parent.plugin == 0 && placement.parent.local_id == 0x3C);
  Check("player position", placement.position[0] == -24067.2969f &&
                               placement.position[1] == 97838.0469f &&
                               placement.position[2] == -13344.8779f);

  // Nothing from a plugin the game does not have may reach the sink.
  RuntimeOrder without;
  without.plugins.push_back("Skyrim.esm");
  const FormRemap partial = BuildRemap(save, without);
  RecordingSink refused_sink;
  SaveApplyStats refused_stats;
  rx::bethesda::ApplySave(save, partial, refused_sink, &refused_stats);
  Check("mod's global refused", refused_sink.globals.size() == 1);
  Check("mod's reference refused", refused_sink.moves.empty() && refused_sink.enabled_refs.empty());
  Check("refusals counted", refused_stats.forms.missing_plugin == 3);
  Check("alias fill from the missing plugin dropped", refused_sink.aliases.empty());
  Check("base game quest still applied", refused_sink.quests.size() == 1);
}

// The level a save reports is only half the answer: a level-mult actor stores a
// 1000-based multiplier of the player's level and a range to clamp it into.
void TestActorLevels() {
  using rx::bethesda::kActorBaseFlagLevelMult;
  using rx::bethesda::ResolveActorLevel;
  const u32 mult = kActorBaseFlagLevelMult;

  Check("a flat level ignores the player", ResolveActorLevel(0, 30, 0, 30, 271) == 30);
  Check("a flat level of zero is still level 1", ResolveActorLevel(0, 0, 0, 0, 271) == 1);
  // Ahtar: 1.0x, 6..30. A level 271 player puts him at his ceiling.
  Check("1.0x clamps to the calc maximum", ResolveActorLevel(mult, 1000, 6, 30, 271) == 30);
  Check("1.0x below the floor clamps up", ResolveActorLevel(mult, 1000, 6, 30, 2) == 6);
  Check("1.0x inside the range is the player's level",
        ResolveActorLevel(mult, 1000, 6, 30, 17) == 17);
  Check("1.2x scales before clamping", ResolveActorLevel(mult, 1200, 10, 100, 20) == 24);
  Check("a zero maximum is no ceiling", ResolveActorLevel(mult, 1000, 0, 0, 271) == 271);
}

// A real 11 MB, 100% complete Skyrim SE save. Not in the repo; point
// RX_SAVEGAME_TEST_FILE at one to run this elsewhere.
constexpr const char kRealSavePath[] =
    "/home/vince/Documents/Projects/recreation/Skyrim Special Edition 100 Percent Complete "
    "Save-53504-1-0-1628477233/pawelos4.ess";

bool ReadWholeFile(const char* path, base::Vector<u8>* out) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  out->resize(size_t(size));
  const bool ok = std::fread(out->data(), 1, size_t(size), f) == size_t(size);
  std::fclose(f);
  return ok;
}

void TestRealSave() {
  const char* path = std::getenv("RX_SAVEGAME_TEST_FILE");
  if (!path)
    path = kRealSavePath;

  base::Vector<u8> file;
  if (!ReadWholeFile(path, &file)) {
    std::printf("real skyrim se save: skipped, %s not present\n", path);
    return;
  }
  std::puts("real skyrim se save");
  SaveFile save;
  if (!rx::bethesda::ReadSaveFile(rx::ByteSpan(file.data(), file.size()), save)) {
    Check("parses", false);
    return;
  }

  // The load order this save was written under, as the game would load it.
  RuntimeOrder order;
  for (const base::String& plugin : save.plugins)
    order.plugins.push_back(plugin);
  const FormRemap remap = BuildRemap(save, order);
  Check("no plugin missing", remap.missing_plugins().empty());

  rx::bethesda::PlayerPlacement placement;
  Check("player placement found", rx::bethesda::FindPlayerPlacement(save, remap, &placement));
  Check("player is in Tamriel (0x3c of the first master)",
        placement.parent.plugin == 0 && placement.parent.local_id == 0x0000003C);
  Check("player position", placement.position[0] == -24067.2969f &&
                               placement.position[1] == 97838.0469f &&
                               placement.position[2] == -13344.8779f);

  RecordingSink sink;
  SaveApplyStats stats;
  rx::bethesda::ApplySave(save, remap, sink, &stats);
  Check("933 globals applied", stats.globals == 933);
  Check("1547 quests applied", stats.quests == 1547);
  Check("4450 quest stages had run", stats.quest_stages == 4450);
  Check("1431 objectives applied", stats.quest_objectives == 1431);
  Check("926 actor bases applied", stats.actors == 926);
  Check("405 actor levels applied", stats.actor_levels == 405);
  Check("608 ai profiles applied", stats.actor_ai_profiles == 608);
  Check("72 crime factions carry infamy", stats.faction_infamy == 72);
  Check("9822 spoken dialogue lines applied", stats.dialogue_said == 9822);
  Check("11153 visited map grids applied", stats.cells_visited == 11153);

  // Levels come out resolved. The save's player is level 271, so a level-mult
  // actor lands at 271 * mult / 1000 clamped to its own calc range.
  const RecordingSink::Level* ahtar = sink.FindLevel(0x0001325F);  // Ahtar, mult 1.0x, 6..30
  Check("Ahtar caps at his calc maximum of 30", ahtar && ahtar->level == 30);
  const RecordingSink::Level* tullius = sink.FindLevel(0x0001327E);  // Tullius, 1.2x, 10..50
  Check("General Tullius caps at his calc maximum of 50", tullius && tullius->level == 50);
  const RecordingSink::Level* elenwen = sink.FindLevel(0x00013269);  // Elenwen, flat 30
  Check("Elenwen keeps her flat level 30", elenwen && elenwen->level == 30);
  const RecordingSink::Level* player = sink.FindLevel(0x00000007);
  Check("the player base carries the save's level 271",
        player && player->level == save.player_level && player->level == 271);

  // Ahtar's AIDT, byte for byte what his NPC_ record authors.
  const RecordingSink::Ai* ahtar_ai = sink.FindAi(0x0001325F);
  Check("Ahtar is unaggressive, brave, moral and helps friends",
        ahtar_ai && ahtar_ai->ai.aggression == 0 && ahtar_ai->ai.confidence == 3 &&
            ahtar_ai->ai.energy == 50 && ahtar_ai->ai.morality == 3 && ahtar_ai->ai.mood == 7 &&
            ahtar_ai->ai.assistance == 2);

  // Infamy, violent first. CrimeFactionImperial authors an all-zero CRVA, so no
  // crime against it is ever worth gold: its 73 can only be a crime count.
  const RecordingSink::Infamy* whiterun = sink.FindInfamy(0x000267EA);
  Check("CrimeFactionWhiterun has seen 44 violent and 247 non-violent crimes",
        whiterun && whiterun->violent == 44 && whiterun->non_violent == 247);
  const RecordingSink::Infamy* imperial = sink.FindInfamy(0x00028848);
  Check("CrimeFactionImperial has seen 73 violent crimes and no theft",
        imperial && imperial->violent == 73 && imperial->non_violent == 0);
  const RecordingSink::Infamy* thieves = sink.FindInfamy(0x0010A794);
  Check("CrimeFactionThievesGuild has seen 18 thefts and no violence",
        thieves && thieves->violent == 0 && thieves->non_violent == 18);

  // Map exploration: one bare 16x16 grid per exterior cell, several masked ones
  // per interior.
  const RecordingSink::Visited* icerunner = sink.FindVisited(0x00009251);
  Check("IcerunnerExterior01 uncovered 220 of its 256 map tiles",
        icerunner && icerunner->grids == 1 && icerunner->tiles == 220);
  const RecordingSink::Visited* inn = sink.FindVisited(0x000133C6);
  Check("RiverwoodSleepingGiantInn carries nine local-map grids",
        inn && inn->grids == 9);

  Check("the line closing FFRiften10 (INFO 0x00013629) was spoken", sink.Said(0x00013629));
  Check("no id landed out of range", stats.forms.out_of_range == 0);
  Check("no plugin missing in the tally", stats.forms.missing_plugin == 0);
  // What is left over is entirely forms the save itself created at runtime,
  // which no plugin defines and this layer therefore cannot place.
  Check("23280 change forms dropped as runtime-created", stats.refused == 23280);

  // Dragonborn.esm is index 4 in this save, so dropping it must refuse every
  // 0x04xxxxxx id and leave the rest untouched.
  RuntimeOrder without;
  for (size_t i = 0; i + 1 < save.plugins.size(); ++i)
    without.plugins.push_back(save.plugins[i]);
  const FormRemap partial = BuildRemap(save, without);
  Check("dropped plugin reported",
        partial.missing_plugins().size() == 1 && partial.missing_plugins()[0] == "Dragonborn.esm");
  RecordingSink partial_sink;
  SaveApplyStats partial_stats;
  rx::bethesda::ApplySave(save, partial, partial_sink, &partial_stats);
  Check("ids from the dropped plugin refused", partial_stats.forms.missing_plugin > 0);
  Check("fewer quests applied without it", partial_stats.quests < stats.quests);
  // The refusal must be all that changed: an id the shorter order still accepts
  // has to land on exactly the record it landed on before.
  size_t agreed = 0, disagreed = 0;
  for (u32 raw : save.form_ids) {
    GlobalFormId full_id, partial_id;
    if (!partial.Map(raw, &partial_id))
      continue;
    if (!remap.Map(raw, &full_id) || !(full_id == partial_id))
      ++disagreed;
    else
      ++agreed;
  }
  Check("surviving ids resolve identically", disagreed == 0 && agreed > 0);

  std::printf(
      "  applied: %u globals, %u quests (%u stages, %u objectives, %u alias fills), %u actors "
      "(%u values, %u faction ranks), %u faction reactions, %u refs moved, %u disabled\n",
      stats.globals, stats.quests, stats.quest_stages, stats.quest_objectives, stats.quest_aliases,
      stats.actors, stats.actor_values, stats.actor_faction_ranks, stats.faction_reactions,
      stats.references_moved, stats.references_disabled);
  std::printf(
      "  also applied: %u visited cell grids, %u spoken infos, %u faction infamy counts, "
      "%u ai profiles, %u actor levels\n",
      stats.cells_visited, stats.dialogue_said, stats.faction_infamy, stats.actor_ai_profiles,
      stats.actor_levels);
  std::printf("  carried but not applied: %u detached cells, %u inventories\n",
              stats.cells_detached, stats.inventories);
  std::printf(
      "  form ids: %u mapped, %u created at runtime, %u refused; %u change forms dropped, "
      "%u undecoded\n",
      stats.forms.mapped, stats.forms.created,
      stats.forms.missing_plugin + stats.forms.out_of_range, stats.refused, stats.undecoded);
}

}  // namespace

int main() {
  std::puts("savegame_applytest");
  TestRemap();
  TestApplySynthetic();
  TestActorLevels();
  TestRealSave();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}

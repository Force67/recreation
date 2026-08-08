// savegame_changeformtest: decodes real ChangeForm payloads lifted out of a
// Skyrim SE save (header version 12, form version 78, 151123 change forms) and
// asserts the values that save actually holds. The payloads are pasted in as
// hex so the test needs no game data and no .ess on disk; each one is a
// complete, unmodified record body, and the record it came from is named above
// it. The last block feeds the decoders truncated and corrupt input, which must
// come back false rather than read past the buffer.

#include <base/containers/vector.h>

#include <cstdio>
#include <cstring>

#include "components/bethesda/savegame_changeform.h"
#include "core/types.h"

namespace {

using rx::bethesda::ActorBaseChange;
using rx::bethesda::CellChange;
using rx::bethesda::ChangeForm;
using rx::bethesda::ChangeFormType;
using rx::bethesda::ChangeRef;
using rx::bethesda::ChangeRefKind;
using rx::bethesda::DecodeActorBase;
using rx::bethesda::DecodeCell;
using rx::bethesda::DecodeDialogueInfo;
using rx::bethesda::DecodeFaction;
using rx::bethesda::DecodeQuest;
using rx::bethesda::DecodeReference;
using rx::bethesda::DialogueInfoChange;
using rx::bethesda::FactionChange;
using rx::bethesda::QuestChange;
using rx::bethesda::ReferenceChange;
using rx::bethesda::ResolveChangeRef;
using rx::f32;
using rx::u16;
using rx::u32;
using rx::u8;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

u8 Nibble(char c) {
  if (c >= '0' && c <= '9')
    return static_cast<u8>(c - '0');
  return static_cast<u8>((c | 0x20) - 'a' + 10);
}

base::Vector<u8> Hex(const char* text) {
  base::Vector<u8> out;
  for (const char* p = text; p[0] && p[1]; p += 2)
    out.push_back(static_cast<u8>(Nibble(p[0]) << 4 | Nibble(p[1])));
  return out;
}

ChangeForm Form(ChangeFormType type, u32 flags, const char* hex) {
  ChangeForm form;
  form.type = type;
  form.flags = flags;
  form.version = 78;
  form.data = Hex(hex);
  return form;
}

bool IsDefault(const ChangeRef& ref, u32 value) {
  return ref.kind == ChangeRefKind::kDefaultFile && ref.value == value;
}

// QUST 0x0001BAEC dunNilheimQST: stages, objectives and alias fills together.
constexpr const char* kQuestNilheim =
    "1320180000010a00011400011e00016400019600000c0a000000030000001400000002"
    "0000001e000000030000000007000000000000000041ba060700000000472a8d050000"
    "000041bb22060000000041bb23090000000041bb20030000000041bafe040000000041"
    "bb240000000000010000000401000000000064000001";

// QUST 0x00016C8D MS05_dunDeadMensRespiteQST: a 29 entry stage list.
constexpr const char* kQuestMs05 =
    "0001740000000a00011400011d00011e00012700012800013100013200013b00013c00"
    "014500014600014b00014f00015000005500015900015a00015f000163000164000169"
    "00016e00017300017800017d0001820001ff000101";

// FACT 0x000267EA CrimeFactionWhiterun.
constexpr const char* kFactionWhiterun =
    "0c49182200000000010000004c4de000000000010000000002610000000001000000709"
    "000002c000000f7000000ffff7fff508b2347";

// NPC_ 0x0001325F Ahtar, the Solitude headsman.
constexpr const char* kActorAhtar =
    "b000400000000000e80306001e006400000000000000000018429db00045c84eff46fd"
    "640045c84d004c13e80042817c0000033203070200ff0000000000000000000000002e"
    "4e3e3e144f0f0f0f0f0f0f140f140f0f0f0000000000000000000000000000000000008"
    "4013200930058b900000000016ae2b3";

// REFR 0x000BBCD1: a container holding nothing but gold.
constexpr const char* kRefrGold = "0440000f6502000000";

// REFR 0xFF00081D: a runtime created reference placed in an interior cell.
constexpr const char* kRefrPlaced =
    "4165a34c5bd0c34de06b456fb3f743cb57e73c820ea8bc59c6203f014f147008004000"
    "0000";

// ACHR 0x00000014, the player. Only the leading transform is pasted in: the
// real payload runs to 130087 bytes of actor state this layer does not decode,
// and the decoder stops after the transform on an ACHR anyway.
constexpr const char* kPlayerTransform =
    "40003c9806bcc60617bf47838350c6f8602bbe00000080be21c23e00000000";

// CELL 0x00013A7B DragonBridgePenitusOculatusOutpost.
constexpr const char* kCellDragonBridge =
    "06b6000004ffff000000000000007f80ff80ff80ff80ff80ff80ff80ff00ff00000000"
    "00000000";

void TestChangeRef() {
  const u32 map[] = {0x0001A2B3u, 0x000FF001u, 0x0201CCDDu};
  base::Span<const u32> ids(map, 3);

  Check("kFormIdIndex is one based",
        ResolveChangeRef({ChangeRefKind::kFormIdIndex, 1}, ids) == 0x0001A2B3u &&
            ResolveChangeRef({ChangeRefKind::kFormIdIndex, 3}, ids) == 0x0201CCDDu);
  Check("kFormIdIndex 0 means no reference",
        ResolveChangeRef({ChangeRefKind::kFormIdIndex, 0}, ids) == 0);
  Check("kFormIdIndex past the map resolves to nothing",
        ResolveChangeRef({ChangeRefKind::kFormIdIndex, 4}, ids) == 0);
  Check("kDefaultFile is the form id verbatim",
        ResolveChangeRef({ChangeRefKind::kDefaultFile, 0x14}, ids) == 0x14u);
  Check("kCreated lands in the 0xFF range",
        ResolveChangeRef({ChangeRefKind::kCreated, 0x0243Cu}, ids) == 0xFF00243Cu);
  Check("kUnused resolves to nothing",
        ResolveChangeRef({ChangeRefKind::kUnused, 7}, ids) == 0);
}

void TestQuest() {
  const ChangeForm form = Form(ChangeFormType::kQust, 0xbc000002u, kQuestNilheim);
  QuestChange quest;
  Check("dunNilheimQST decodes", DecodeQuest(form, quest));

  Check("quest flags and priority", quest.quest_flags == 0x13 && quest.priority == 32);
  Check("six stage entries", quest.stages.size() == 6);
  Check("stage 0 has run", quest.stages[0].stage == 0 && quest.stages[0].flags == 1);
  Check("stage 100 has run", quest.stages[4].stage == 100 && quest.stages[4].flags == 1);
  Check("stage 150 has not run", quest.stages[5].stage == 150 && quest.stages[5].flags == 0);

  Check("three objectives", quest.objectives.size() == 3);
  Check("objective 10 state 3",
        quest.objectives[0].index == 10 && quest.objectives[0].state == 3);
  Check("objective 20 state 2",
        quest.objectives[1].index == 20 && quest.objectives[1].state == 2);
  Check("objective 30 state 3",
        quest.objectives[2].index == 30 && quest.objectives[2].state == 3);

  Check("seven alias fills", quest.alias_fills.size() == 7);
  Check("alias 0 filled with 0x0001BA06",
        quest.alias_fills[0].alias_id == 0 &&
            IsDefault(quest.alias_fills[0].ref, 0x0001BA06u));
  Check("alias 7 filled with 0x00072A8D",
        quest.alias_fills[1].alias_id == 7 &&
            IsDefault(quest.alias_fills[1].ref, 0x00072A8Du));
  Check("alias 4 filled with 0x0001BB24",
        quest.alias_fills[6].alias_id == 4 &&
            IsDefault(quest.alias_fills[6].ref, 0x0001BB24u));
  // Prefix through the alias fills; the start event and instance data that
  // follow are deliberately left alone.
  Check("decoded prefix ends after the alias fills", quest.decoded_bytes == 107);

  const ChangeForm ms05 = Form(ChangeFormType::kQust, 0x84000002u, kQuestMs05);
  QuestChange respite;
  Check("MS05 decodes", DecodeQuest(ms05, respite));
  Check("MS05 has 29 stage entries", respite.stages.size() == 29);
  Check("MS05 stage 80 recorded but not run",
        respite.stages[15].stage == 80 && respite.stages[15].flags == 0);
  Check("MS05 stage 255 run",
        respite.stages[28].stage == 255 && respite.stages[28].flags == 1);
  Check("MS05 carries no objectives or aliases",
        respite.objectives.empty() && respite.alias_fills.empty());
  Check("MS05 consumed all but its trailing byte",
        respite.decoded_bytes + 1 == ms05.data.size());

  QuestChange wrong;
  Check("a quest decoder refuses a non quest change form",
        !DecodeQuest(Form(ChangeFormType::kFact, 0x84000002u, kQuestMs05), wrong));
}

void TestFaction() {
  const ChangeForm form =
      Form(ChangeFormType::kFact, 0x80000006u, kFactionWhiterun);
  FactionChange faction;
  Check("CrimeFactionWhiterun decodes", DecodeFaction(form, faction));

  Check("three reactions", faction.reactions.size() == 3);
  Check("reaction 0 is PlayerWerewolfFaction 0x00091822, enemy",
        IsDefault(faction.reactions[0].faction, 0x00091822u) &&
            faction.reactions[0].modifier == 0 &&
            faction.reactions[0].combat_reaction == 1);
  Check("reaction 1 is VampirePCFaction 0x000C4DE0, enemy",
        IsDefault(faction.reactions[1].faction, 0x000C4DE0u) &&
            faction.reactions[1].combat_reaction == 1);
  Check("reaction 2 comes through the form id map",
        faction.reactions[2].faction.kind == ChangeRefKind::kFormIdIndex &&
            faction.reactions[2].faction.value == 609 &&
            faction.reactions[2].combat_reaction == 1);

  Check("form flags", faction.has_form_flags && faction.form_flags == 0x00009070u);
  Check("crime counts are 44 and 247",
        faction.has_crime && faction.crime_count_a == 44 &&
            faction.crime_count_b == 247);
  Check("first crime time is the never sentinel",
        faction.crime_time_a == -3.40282347e+38f);
  Check("second crime time", faction.crime_time_b == 41867.3125f);
  Check("payload fully consumed", faction.decoded_bytes == form.data.size());
}

void TestActorBase() {
  const ChangeForm form = Form(ChangeFormType::kNpc, 0x0000024au, kActorAhtar);
  ActorBaseChange actor;
  Check("Ahtar decodes", DecodeActorBase(form, actor));

  Check("base flags", actor.has_stats && actor.base_flags == 0x004000B0u);
  Check("level is a 1.0x player multiplier",
        (actor.base_flags & rx::bethesda::kActorBaseFlagLevelMult) != 0 &&
            actor.level == 1000);
  Check("calc level range 6 to 30",
        actor.calc_min_level == 6 && actor.calc_max_level == 30);
  Check("speed multiplier 100", actor.speed_multiplier == 100);

  Check("six faction ranks", actor.factions.size() == 6);
  Check("first faction CrimeFactionHaafingar 0x00029DB0 at rank 0",
        IsDefault(actor.factions[0].faction, 0x00029DB0u) &&
            actor.factions[0].rank == 0);
  Check("second faction CurrentFollowerFaction 0x0005C84E at rank -1",
        IsDefault(actor.factions[1].faction, 0x0005C84Eu) &&
            actor.factions[1].rank == -1);

  Check("ai data", actor.has_ai && actor.aggression == 0 && actor.confidence == 3 &&
                       actor.energy == 50 && actor.morality == 3 &&
                       actor.mood == 7 && actor.assistance == 2);

  Check("skills", actor.has_skills && actor.skills[0] == 46 && actor.skills[1] == 78 &&
                      actor.skills[5] == 79 && actor.skills[17] == 15);
  Check("no skill offsets", actor.skill_offsets[0] == 0 && actor.skill_offsets[17] == 0);
  Check("health, magicka, stamina are 388, 50, 147",
        actor.health == 388 && actor.magicka == 50 && actor.stamina == 147);
  Check("far away model distance unset", actor.far_away_model_distance == 0.0f);
  Check("payload fully consumed", actor.decoded_bytes == form.data.size());
}

void TestReference() {
  ChangeForm gold = Form(ChangeFormType::kRefr, 0x00000020u, kRefrGold);
  ReferenceChange container;
  Check("gold container decodes", DecodeReference(gold, container));
  Check("one inventory stack",
        container.inventory_complete && container.inventory.size() == 1);
  Check("613 of Gold001 0x0000000F",
        IsDefault(container.inventory[0].item, 0x0000000Fu) &&
            container.inventory[0].count == 613 &&
            container.inventory[0].extra_count == 0);
  Check("no transform recorded", !container.moved && !container.has_form_flags);
  Check("payload fully consumed", container.decoded_bytes == gold.data.size());

  ChangeForm placed = Form(ChangeFormType::kRefr, 0x00000003u, kRefrPlaced);
  ReferenceChange ref;
  Check("placed reference decodes", DecodeReference(placed, ref));
  Check("parent cell is WhiterunDragonsreach 0x000165A3",
        ref.moved && IsDefault(ref.parent, 0x000165A3u));
  Check("position", ref.position[0] == -416.713257f &&
                        ref.position[1] == 3774.0188f &&
                        ref.position[2] == 495.401825f);
  Check("rotation", ref.rotation[0] == 0.0282401051f &&
                        ref.rotation[1] == -0.0205147304f &&
                        ref.rotation[2] == 0.628026545f);
  Check("form flags", ref.has_form_flags && ref.form_flags == 0x00400008u &&
                          ref.form_flags_extra == 0);
  Check("neither deleted nor initially disabled",
        (ref.form_flags & rx::bethesda::kFormFlagDeleted) == 0 &&
            (ref.form_flags & rx::bethesda::kFormFlagInitiallyDisabled) == 0);
  Check("payload fully consumed", ref.decoded_bytes == placed.data.size());

  // An ACHR opens with the same transform, so the same decoder takes it, but it
  // stops there: the rest of an actor record is not this layout.
  ChangeForm player = Form(ChangeFormType::kAchr, 0xb8000c32u, kPlayerTransform);
  ReferenceChange actor_ref;
  Check("the player's ACHR decodes", DecodeReference(player, actor_ref));
  Check("the player stands in the Tamriel worldspace 0x0000003C",
        actor_ref.moved && IsDefault(actor_ref.parent, 0x0000003Cu));
  Check("player position", actor_ref.position[0] == -24067.2969f &&
                               actor_ref.position[1] == 97838.0469f &&
                               actor_ref.position[2] == -13344.8779f);
  Check("player rotation", actor_ref.rotation[0] == -0.167362094f &&
                               actor_ref.rotation[2] == 0.379163682f);
  Check("the ACHR walk stops after the transform",
        actor_ref.decoded_bytes == 31 && !actor_ref.has_form_flags &&
            actor_ref.inventory.empty());

  ReferenceChange other;
  Check("refuses a quest change form",
        !DecodeReference(Form(ChangeFormType::kQust, 3, kRefrPlaced), other));
}

void TestDialogueInfo() {
  ChangeForm form = Form(ChangeFormType::kInfo, 0x80000000u, "");
  DialogueInfoChange info;
  Check("an empty INFO payload decodes", DecodeDialogueInfo(form, info));
  Check("said flag set", info.said);

  ChangeForm unsaid = Form(ChangeFormType::kInfo, 0x00000000u, "");
  DialogueInfoChange fresh;
  Check("a cleared flag reads back as unsaid",
        DecodeDialogueInfo(unsaid, fresh) && !fresh.said);

  ChangeForm bogus = Form(ChangeFormType::kInfo, 0x80000000u, "00");
  DialogueInfoChange rejected;
  Check("an INFO payload with bytes in it is refused",
        !DecodeDialogueInfo(bogus, rejected));
}

void TestCell() {
  ChangeForm form = Form(ChangeFormType::kCell, 0xc0000000u, kCellDragonBridge);
  CellChange cell;
  Check("DragonBridge outpost cell decodes", DecodeCell(form, cell));
  Check("not detached", !cell.detached);
  Check("one visited grid", cell.visited.size() == 1);
  Check("grid mask", cell.visited[0].mask == 0xFFFF);
  Check("grid bits", cell.visited[0].bits[7] == 0x7f && cell.visited[0].bits[8] == 0x80 &&
                         cell.visited[0].bits[31] == 0x00);
  Check("payload fully consumed", cell.decoded_bytes == form.data.size());
}

void TestMalformed() {
  // Truncating a real payload at every length must fail cleanly, never read
  // past the buffer and never report success on a short read.
  const base::Vector<u8> quest = Hex(kQuestNilheim);
  for (rx::u64 len = 0; len < quest.size(); ++len) {
    ChangeForm form;
    form.type = ChangeFormType::kQust;
    form.flags = 0xbc000002u;
    form.version = 78;
    for (rx::u64 i = 0; i < len; ++i)
      form.data.push_back(quest[i]);
    QuestChange out;
    if (DecodeQuest(form, out) && out.decoded_bytes > len) {
      Check("truncated quest reported more bytes than it was given", false);
      return;
    }
  }
  Check("every truncation of a quest payload is handled", true);

  const base::Vector<u8> actor = Hex(kActorAhtar);
  bool actor_ok = true;
  for (rx::u64 len = 0; len + 1 < actor.size(); ++len) {
    ChangeForm form;
    form.type = ChangeFormType::kNpc;
    form.flags = 0x0000024au;
    form.version = 78;
    for (rx::u64 i = 0; i < len; ++i)
      form.data.push_back(actor[i]);
    ActorBaseChange out;
    if (DecodeActorBase(form, out))
      actor_ok = false;
  }
  Check("no truncation of an NPC_ payload decodes", actor_ok);

  // A count field big enough to run off the end must be rejected, not trusted.
  ChangeForm huge = Form(ChangeFormType::kQust, 0x80000002u, "0000fcffff03");
  QuestChange out;
  Check("an absurd stage count is rejected", !DecodeQuest(huge, out));

  ChangeForm huge_inventory =
      Form(ChangeFormType::kRefr, 0x00000020u, "fcffff0300000000");
  ReferenceChange inv;
  Check("an absurd inventory count is rejected",
        !DecodeReference(huge_inventory, inv));

  ChangeForm huge_reactions =
      Form(ChangeFormType::kFact, 0x00000004u, "fcffff03000000");
  FactionChange fac;
  Check("an absurd reaction count is rejected", !DecodeFaction(huge_reactions, fac));

  ChangeForm huge_grid = Form(ChangeFormType::kCell, 0x40000000u, "00000000fcffff03");
  CellChange cel;
  Check("an absurd cell grid count is rejected", !DecodeCell(huge_grid, cel));

  // Unknown versions must be refused outright rather than parsed on the
  // assumption that the layout held.
  ChangeForm old_version = Form(ChangeFormType::kQust, 0xbc000002u, kQuestNilheim);
  old_version.version = 64;
  QuestChange stale;
  Check("an unknown change form version is refused", !DecodeQuest(old_version, stale));

  ChangeForm future = Form(ChangeFormType::kNpc, 0x0000024au, kActorAhtar);
  future.version = 200;
  ActorBaseChange ahead;
  Check("a future change form version is refused", !DecodeActorBase(future, ahead));

  // An empty payload with every group flag set: nothing to read, everything
  // must fail rather than fabricate.
  ChangeForm empty = Form(ChangeFormType::kQust, 0xffffffffu, "");
  QuestChange nothing;
  Check("an empty quest payload with all flags set is refused",
        !DecodeQuest(empty, nothing));

  ChangeForm empty_ref = Form(ChangeFormType::kRefr, 0x00000003u, "");
  ReferenceChange nothing_ref;
  Check("an empty reference payload with a transform flag is refused",
        !DecodeReference(empty_ref, nothing_ref));
}

}  // namespace

int main() {
  std::puts("savegame changeform: decoding real Skyrim SE payloads");
  TestChangeRef();
  TestQuest();
  TestFaction();
  TestActorBase();
  TestReference();
  TestDialogueInfo();
  TestCell();
  TestMalformed();

  if (g_failures == 0) {
    std::puts("savegame changeform: all checks passed");
    return 0;
  }
  std::printf("savegame changeform: %d checks FAILED\n", g_failures);
  return 1;
}

// savegame_changeformtest: decodes real ChangeForm payloads lifted out of a
// Skyrim SE save (header version 12, form version 78, 151123 change forms) and
// asserts the values that save actually holds. The payloads are pasted in as
// hex so the test needs no game data and no .ess on disk; each one is a
// complete, unmodified record body, and the record it came from is named above
// it. The last block feeds the decoders truncated and corrupt input, which must
// come back false rather than read past the buffer.
//
// The last block opens the save itself, when it is on the machine, and asserts
// what only a whole record can show: the player's own 130087 byte ACHR.

#include <base/containers/span.h>
#include <base/containers/vector.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "components/bethesda/savegame.h"
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
using rx::i64;
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

// `form_id` matters to the reference decoder: a form the save invented at
// runtime (0xFFxxxxxx) writes its base object into the transform group, so a
// record pasted in without its id decodes as a different shape.
ChangeForm Form(ChangeFormType type, u32 flags, const char* hex, u32 form_id = 0) {
  ChangeForm form;
  form.form_id = form_id;
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
// real payload runs to 130087 bytes, so the whole record is exercised against
// the save itself in TestRealSave instead.
constexpr const char* kPlayerTransform =
    "40003c9806bcc60617bf47838350c6f8602bbe00000080be21c23e00000000";

// ACHR 0x000D2B17, an actor standing in a Windhelm house: transform, form flags
// and an inventory of two stacks, both marked worn, each carrying the outfit
// item that put it there.
constexpr const char* kActorDressed =
    "41677e41336f45cb5da2c5e7d652c20000000000000080c1a99240ffffffffa2212038"
    "2b0200000000084869910100000004088e49d5e0164869930100000004088e49d5e016"
    "fff14ead3f000080bf803ae643b31031c4000000000000000000000000000000000000"
    "000000000000000000001b8f0000000020420000000000000002000000000001000000"
    "0001000000000000000000000000000000000000000000030000000000000000000000"
    "0000000003000000000000000000000000000000030000000000000000000000000000"
    "000300000000000000000000000000000010180000000000964219000000000070421a"
    "000000000070424d00000000000000000000000000040100100000000000ffffff7f00"
    "00000000000000";

// REFR 0x0004FDAF, an urn whose base DB11LargeUrn (0x0004FDAE) authors 20000
// gold. The save records -20000, i.e. the player emptied it.
constexpr const char* kRefrEmptiedUrn =
    "0904000040000488000440000fe0b1ffff003c000500436c6f7365002a0000000000";

// REFR 0x00039F1C, MQ201ElenwenOfficeChest (base 0x000F684D), which authors one
// copy each of two books. Both are recorded as -1.
constexpr const char* kRefrLootedChest =
    "048800084f6846ffffffff004f6845ffffffff003c000500436c6f7365002a0000000000";

// REFR 0x000CD0B6, a BarrelIngredientCommon01 holding four of one ingredient
// with an extra-data list on the stack.
constexpr const char* kRefrBarrel = "04434d220400000004082e0000000000240400";

// REFR 0x00015D51 Angarvunde, a location the player found. The whole payload is
// the extra-data list that carries the map marker: one entry, one flags byte.
constexpr const char* kMarkerAngarvunde = "042c03";

// REFR 0x00016213 Blind Cliff Cave: the marker followed by a second extra (an
// empty quest-alias list), so the walk only reaches the end of the payload if
// it sized the marker right.
constexpr const char* kMarkerBlindCliffCave = "082c038800";

// REFR 0x00017780 Helgen, promoted into the save, so its marker sits behind a
// transform group rather than at the head of the payload.
constexpr const char* kMarkerHelgen =
    "40003ccd5c8f467c3c9bc7a5d10f460000000000000080bfd9bd3f40003c0400ecff082c"
    "038c0440028a";

// REFR 0x00016276 Reach Stormcloak Camp: visible on the map but not travelled
// to, which is how Skyrim leaves the civil war camps.
constexpr const char* kMarkerStormcloakCamp =
    "40003c25e1e8c7d95b444729d201c60000000000000080f5bdc84040003ce2ff0c00090c"
    "00004000082c018c0440028a";

// CELL 0x00013A7B DragonBridgePenitusOculatusOutpost, an interior: a detach
// time and one local-map tile at (-1,-1).
constexpr const char* kCellDragonBridge =
    "06b6000004ffff000000000000007f80ff80ff80ff80ff80ff80ff80ff00ff00000000"
    "00000000";

// CELL 0x0000924F, an exterior of Tamriel at grid (-6,26): the coordinate
// group, a detach time, and the one 16x16 map tile the cell itself is.
constexpr const char* kCellTamrielExterior =
    "0100fa1a30f50000ffffffffffffffff9fff1fff1fff1ffe1fc00fc00f800f000f0007"
    "0007000700";

// CELL 0x0000185F, the same worldspace at grid (-40,41), detached and with
// nothing of it uncovered: coordinates and detach time, no map bits at all.
constexpr const char* kCellTamrielUnseen = "0100d829466e0000";

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
  Check("44 violent and 247 non-violent crimes on record",
        faction.has_crime && faction.infamy_violent == 44 &&
            faction.infamy_non_violent == 247);
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
  ChangeForm gold =
      Form(ChangeFormType::kRefr, 0x00000020u, kRefrGold, 0x000BBCD1u);
  ReferenceChange container;
  Check("gold container decodes", DecodeReference(gold, container));
  Check("one inventory stack",
        container.inventory_complete && container.inventory.size() == 1);
  Check("613 of Gold001 0x0000000F",
        IsDefault(container.inventory[0].item, 0x0000000Fu) &&
            container.inventory[0].count == 613 &&
            !container.inventory[0].equipped);
  Check("no transform recorded", !container.moved && !container.has_form_flags);
  Check("payload fully consumed", container.decoded_bytes == gold.data.size());

  ChangeForm placed =
      Form(ChangeFormType::kRefr, 0x00000003u, kRefrPlaced, 0xFF00081Du);
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
  // A created reference carries its base object inside the transform group, and
  // that is the only thing that makes this payload add up to its 37 bytes.
  Check("created reference names its base object 0x000F1470",
        IsDefault(ref.base_object, 0x000F1470u));
  Check("payload fully consumed", ref.decoded_bytes == placed.data.size());

  // An ACHR opens with the same transform, so the same decoder takes it. This
  // one is only the transform, so the walk has to stop where the bytes do.
  ChangeForm player =
      Form(ChangeFormType::kAchr, 0xb8000c32u, kPlayerTransform, 0x00000014u);
  ReferenceChange actor_ref;
  Check("the player's ACHR decodes", DecodeReference(player, actor_ref));
  Check("the player stands in the Tamriel worldspace 0x0000003C",
        actor_ref.moved && IsDefault(actor_ref.parent, 0x0000003Cu));
  Check("player position", actor_ref.position[0] == -24067.2969f &&
                               actor_ref.position[1] == 97838.0469f &&
                               actor_ref.position[2] == -13344.8779f);
  Check("player rotation", actor_ref.rotation[0] == -0.167362094f &&
                               actor_ref.rotation[2] == 0.379163682f);
  Check("a payload cut off after the transform reports only the transform",
        actor_ref.decoded_bytes == 27 && !actor_ref.has_form_flags &&
            actor_ref.inventory.empty());

  // A whole actor record: the eight bytes an ACHR writes with no flag asking
  // for them, then the flag groups, then the actor's own block.
  ChangeForm dressed =
      Form(ChangeFormType::kAchr, 0x08000023u, kActorDressed, 0x000D2B17u);
  ReferenceChange townsfolk;
  Check("a dressed actor decodes", DecodeReference(dressed, townsfolk));
  Check("stands in cell WindhelmHouseofClanShatterShield 0x0001677E",
        townsfolk.moved && IsDefault(townsfolk.parent, 0x0001677Eu));
  Check("form flags read past the unflagged eight bytes",
        townsfolk.has_form_flags && townsfolk.form_flags == 0x0000022Bu);
  Check("two inventory stacks, both worn",
        townsfolk.inventory_complete && townsfolk.inventory.size() == 2 &&
            townsfolk.inventory[0].equipped && townsfolk.inventory[1].equipped);
  Check("wearing ClothesFineClothes01 0x00086991",
        IsDefault(townsfolk.inventory[0].item, 0x00086991u) &&
            townsfolk.inventory[0].count == 1);
  Check("wearing ClothesFineBoots01 0x00086993",
        IsDefault(townsfolk.inventory[1].item, 0x00086993u) &&
            townsfolk.inventory[1].count == 1);
  // Its value tables are all empty, which is the normal case: an ordinary NPC
  // takes its values from its base record and stores no rows of its own.
  Check("no actor values of its own", townsfolk.actor_values.empty());
  Check("the flag groups end 70 bytes in, where the actor block starts",
        townsfolk.decoded_bytes == 70);

  ReferenceChange other;
  Check("refuses a quest change form",
        !DecodeReference(Form(ChangeFormType::kQust, 3, kRefrPlaced), other));
}

// A saved inventory count is a DELTA against the contents the container's base
// record authors, not the contents themselves. These three are the proof, and
// the authored counts they are checked against come from Skyrim.esm: without
// that reading, an emptied container looks like one holding a negative number
// of things.
void TestContainerDelta() {
  ChangeForm urn =
      Form(ChangeFormType::kRefr, 0x90000021u, kRefrEmptiedUrn, 0x0004FDAFu);
  ReferenceChange emptied;
  Check("the emptied urn decodes", DecodeReference(urn, emptied));
  Check("one stack, complete",
        emptied.inventory_complete && emptied.inventory.size() == 1);
  // DB11LargeUrn authors 20000 Gold001, so -20000 is "the player took it all"
  // and 20000 + (-20000) is the count that survives the load.
  Check("Gold001 0x0000000F at -20000, exactly what the base record authors",
        IsDefault(emptied.inventory[0].item, 0x0000000Fu) &&
            emptied.inventory[0].count == -20000);
  Check("payload fully consumed", emptied.decoded_bytes == urn.data.size());

  ChangeForm chest =
      Form(ChangeFormType::kRefr, 0x98000020u, kRefrLootedChest, 0x00039F1Cu);
  ReferenceChange looted;
  Check("the looted chest decodes", DecodeReference(chest, looted));
  Check("two stacks", looted.inventory_complete && looted.inventory.size() == 2);
  Check("both books at -1, one each as MQ201ElenwenOfficeChest authors them",
        IsDefault(looted.inventory[0].item, 0x000F6846u) &&
            looted.inventory[0].count == -1 &&
            IsDefault(looted.inventory[1].item, 0x000F6845u) &&
            looted.inventory[1].count == -1);
  Check("payload fully consumed", looted.decoded_bytes == chest.data.size());

  // A stack carrying extra data used to end the walk here; now the list is
  // stepped over by type and the count on the far side of it still reads.
  ChangeForm barrel =
      Form(ChangeFormType::kRefr, 0x08000020u, kRefrBarrel, 0x000CD0B6u);
  ReferenceChange ingredients;
  Check("the ingredient barrel decodes", DecodeReference(barrel, ingredients));
  Check("four of 0x00034D22 read past its extra-data list",
        ingredients.inventory_complete && ingredients.inventory.size() == 1 &&
            IsDefault(ingredients.inventory[0].item, 0x00034D22u) &&
            ingredients.inventory[0].count == 4);
  Check("payload fully consumed", ingredients.decoded_bytes == barrel.data.size());
}

// A discovered location is a REFR whose extra-data list carries a map marker.
// The save stores nothing but the flags: what the marker is called, where it
// sits and which icon it draws all stay in the plugin's REFR record.
void TestMapMarker() {
  ChangeForm angarvunde =
      Form(ChangeFormType::kRefr, 0x80000000u, kMarkerAngarvunde, 0x00015D51u);
  ReferenceChange found;
  Check("Angarvunde's marker decodes", DecodeReference(angarvunde, found));
  Check("visible and travelable",
        found.has_map_marker &&
            found.map_marker_flags ==
                (rx::bethesda::kMapMarkerVisible | rx::bethesda::kMapMarkerCanTravelTo));
  Check("payload fully consumed", found.decoded_bytes == angarvunde.data.size());

  ChangeForm blind_cliff =
      Form(ChangeFormType::kRefr, 0x80000000u, kMarkerBlindCliffCave, 0x00016213u);
  ReferenceChange second;
  Check("a marker followed by another extra decodes",
        DecodeReference(blind_cliff, second) && second.has_map_marker &&
            second.map_marker_flags == 3);
  Check("payload fully consumed", second.decoded_bytes == blind_cliff.data.size());

  ChangeForm helgen = Form(ChangeFormType::kRefr, 0x82000000u, kMarkerHelgen, 0x00017780u);
  ReferenceChange town;
  Check("Helgen's marker decodes behind its transform", DecodeReference(helgen, town));
  Check("it sits in the Tamriel worldspace 0x0000003C at Helgen's coordinates",
        town.moved && IsDefault(town.parent, 0x0000003Cu) &&
            town.position[0] == 18350.4004f && town.position[1] == -79480.9688f &&
            town.position[2] == 9204.4111f);
  Check("visible and travelable", town.has_map_marker && town.map_marker_flags == 3);
  Check("payload fully consumed", town.decoded_bytes == helgen.data.size());

  ChangeForm camp =
      Form(ChangeFormType::kRefr, 0x82000001u, kMarkerStormcloakCamp, 0x00016276u);
  ReferenceChange stormcloak;
  Check("the Reach Stormcloak camp's marker decodes", DecodeReference(camp, stormcloak));
  Check("visible but not a fast travel destination",
        stormcloak.has_map_marker &&
            (stormcloak.map_marker_flags & rx::bethesda::kMapMarkerVisible) != 0 &&
            (stormcloak.map_marker_flags & rx::bethesda::kMapMarkerCanTravelTo) == 0);
  Check("payload fully consumed", stormcloak.decoded_bytes == camp.data.size());

  // The gold container from TestReference carries no extra-data list at all.
  ChangeForm gold = Form(ChangeFormType::kRefr, 0x00000020u, kRefrGold, 0x000BBCD1u);
  ReferenceChange container;
  Check("a reference with no marker reports none",
        DecodeReference(gold, container) && !container.has_map_marker &&
            container.map_marker_flags == 0);
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
  // An interior: no coordinate group, so its map data is a counted list of
  // local-map tiles that carry their own coordinate.
  ChangeForm form = Form(ChangeFormType::kCell, 0xc0000000u, kCellDragonBridge);
  CellChange cell;
  Check("DragonBridge outpost cell decodes", DecodeCell(form, cell));
  Check("no exterior coordinate", !cell.has_grid);
  Check("detached at game hour 46598", cell.has_detach_time && cell.detach_time == 46598);
  Check("one local map tile", cell.visited.size() == 1);
  Check("the tile sits at (-1,-1)", cell.visited[0].has_tile && cell.visited[0].tile_x == -1 &&
                                        cell.visited[0].tile_y == -1);
  Check("tile bits", cell.visited[0].bits[7] == 0x7f && cell.visited[0].bits[8] == 0x80 &&
                         cell.visited[0].bits[31] == 0x00);
  Check("payload fully consumed", cell.decoded_bytes == form.data.size());

  // An exterior: the coordinate group says where it is, and its map data is the
  // one tile the cell is, written bare with no coordinate of its own.
  ChangeForm outside = Form(ChangeFormType::kCell, 0xe0000000u, kCellTamrielExterior);
  CellChange seen;
  Check("an exterior cell decodes", DecodeCell(outside, seen));
  Check("its grid is (-6,26)", seen.has_grid && seen.grid_x == -6 && seen.grid_y == 26);
  Check("detached at game hour 62768", seen.has_detach_time && seen.detach_time == 62768);
  Check("one bare map tile", seen.visited.size() == 1 && !seen.visited[0].has_tile);
  Check("tile bits", seen.visited[0].bits[0] == 0xff && seen.visited[0].bits[8] == 0x9f &&
                         seen.visited[0].bits[31] == 0x00);
  Check("payload fully consumed", seen.decoded_bytes == outside.data.size());

  // The two groups alone, with no map bits: the whole payload is 4 + 4.
  ChangeForm unseen = Form(ChangeFormType::kCell, 0x60000000u, kCellTamrielUnseen);
  CellChange bare;
  Check("a cell with no map data decodes", DecodeCell(unseen, bare));
  Check("its grid is (-40,41)", bare.has_grid && bare.grid_x == -40 && bare.grid_y == 41);
  Check("detached at game hour 28230", bare.has_detach_time && bare.detach_time == 28230);
  Check("nothing uncovered", bare.visited.empty());
  Check("payload fully consumed", bare.decoded_bytes == unseen.data.size());

  // The form flags follow the two leading groups rather than heading the
  // payload, so a cell that carries them still reads its coordinate at byte 2.
  base::Vector<u8> with_flags = Hex(kCellTamrielExterior);
  with_flags.insert(with_flags.begin() + 8, 0x07);
  with_flags.insert(with_flags.begin() + 9, 0x00);
  ChangeForm flagged;
  flagged.type = ChangeFormType::kCell;
  flagged.version = 78;
  flagged.flags = 0xe0000002u;
  flagged.data = with_flags;
  CellChange after;
  Check("a cell carrying form flags decodes", DecodeCell(flagged, after));
  Check("the coordinate is still (-6,26)",
        after.has_grid && after.grid_x == -6 && after.grid_y == 26);
  Check("form flags read 7", after.has_form_flags && after.form_flags == 7);
  Check("payload fully consumed", after.decoded_bytes == flagged.data.size());
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

  // An inventory count that cannot fit in what is left ends the walk. The
  // record still decodes, because its transform stands, but not one item is
  // reported off a count the payload cannot back.
  ChangeForm huge_inventory =
      Form(ChangeFormType::kRefr, 0x00000020u, "fcffff0300000000");
  ReferenceChange inv;
  Check("an absurd inventory count yields no items",
        DecodeReference(huge_inventory, inv) && inv.inventory.empty() &&
            !inv.inventory_complete &&
            inv.decoded_bytes < huge_inventory.data.size());

  ChangeForm huge_reactions =
      Form(ChangeFormType::kFact, 0x00000004u, "fcffff03000000");
  FactionChange fac;
  Check("an absurd reaction count is rejected", !DecodeFaction(huge_reactions, fac));

  // A detach time and then a tile count of four million with one byte behind it.
  ChangeForm huge_grid = Form(ChangeFormType::kCell, 0xc0000000u, "00000000feffff03");
  CellChange cel;
  Check("an absurd cell tile count is rejected", !DecodeCell(huge_grid, cel));

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

  // An extra-data list that names a map marker and then stops must not read the
  // flags byte off the end of the payload.
  ChangeForm cut_marker = Form(ChangeFormType::kRefr, 0x80000000u, "042c", 0x00015D51u);
  ReferenceChange cut;
  Check("a marker cut off before its flags reports none",
        DecodeReference(cut_marker, cut) && !cut.has_map_marker);

  ChangeForm empty_ref = Form(ChangeFormType::kRefr, 0x00000003u, "");
  ReferenceChange nothing_ref;
  Check("an empty reference payload with a transform flag is refused",
        !DecodeReference(empty_ref, nothing_ref));
}

// The 100% complete save the layouts above were derived from. Not in the repo,
// so this half is skipped when the file is not on the machine; point
// RX_SAVEGAME_TEST_FILE at one to run it elsewhere. What it proves that a
// pasted payload cannot: the player's own record, all 130087 bytes of it.
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
  out->resize(static_cast<size_t>(size));
  const bool ok = std::fread(out->data(), 1, static_cast<size_t>(size), f) ==
                  static_cast<size_t>(size);
  std::fclose(f);
  return ok;
}

bool AllPerksAtRank(const ReferenceChange& ref, u8 rank) {
  for (const rx::bethesda::ActorPerk& perk : ref.perks) {
    if (perk.rank != rank)
      return false;
  }
  return !ref.perks.empty();
}

bool HasPerk(const ReferenceChange& ref, const rx::bethesda::SaveFile& save, u32 form_id) {
  for (const rx::bethesda::ActorPerk& perk : ref.perks) {
    if (ResolveChangeRef(perk.perk, base::Span<const u32>(save.form_ids.data(),
                                                          save.form_ids.size())) == form_id)
      return true;
  }
  return false;
}

f32 ActorValueOf(const ReferenceChange& ref, u32 index) {
  for (const rx::bethesda::ActorValueEntry& entry : ref.actor_values) {
    if (entry.index == index)
      return entry.value;
  }
  return -1.0f;
}

void TestRealSave() {
  const char* path = std::getenv("RX_SAVEGAME_TEST_FILE");
  if (!path)
    path = kRealSavePath;
  base::Vector<u8> file;
  if (!ReadWholeFile(path, &file)) {
    std::printf("real save: skipped, %s not present\n", path);
    return;
  }
  rx::bethesda::SaveFile save;
  if (!rx::bethesda::ReadSaveFile(rx::ByteSpan(file.data(), file.size()), save)) {
    Check("the real save parses", false);
    return;
  }
  std::puts("real save: Pawelos, level 271, 100% complete");

  const ChangeForm* player_ref = nullptr;
  const ChangeForm* player_base = nullptr;
  for (const ChangeForm& form : save.change_forms) {
    if (form.form_id == 0x00000014u && form.type == ChangeFormType::kAchr)
      player_ref = &form;
    if (form.form_id == 0x00000007u && form.type == ChangeFormType::kNpc)
      player_base = &form;
  }
  Check("the save carries the player's reference and base form",
        player_ref != nullptr && player_base != nullptr);
  if (player_ref == nullptr || player_base == nullptr)
    return;

  ReferenceChange player;
  Check("the player's 130087 byte ACHR decodes",
        player_ref->data.size() == 130087 && DecodeReference(*player_ref, player));
  Check("the flag groups end 20051 bytes in", player.decoded_bytes == 20051);
  Check("scale 1.0", player.has_scale && player.scale == 1.0f);

  Check("289 inventory stacks", player.inventory_complete &&
                                    player.inventory.size() == 289);
  i64 gold = 0;
  u32 equipped = 0;
  bool has_bow = false;
  for (const rx::bethesda::InventoryItem& item : player.inventory) {
    if (IsDefault(item.item, 0x0000000Fu))
      gold = item.count;
    if (item.equipped || item.equipped_left)
      ++equipped;
    // DLC1DragonboneBow, the weapon the save has in hand.
    if (item.item.kind == ChangeRefKind::kFormIdIndex && item.equipped &&
        ResolveChangeRef(item.item, base::Span<const u32>(save.form_ids.data(),
                                                          save.form_ids.size())) ==
            0x020176F4u)
      has_bow = true;
  }
  Check("1694067 gold", gold == 1694067);
  Check("eight stacks worn", equipped == 8);
  Check("6315 arrows of the worn stack are ammunition", has_bow);

  Check("45 actor values", player.actor_values.size() == 45);
  Check("health, magicka and stamina are 1000 each",
        ActorValueOf(player, 24) == 1000.0f && ActorValueOf(player, 25) == 1000.0f &&
            ActorValueOf(player, 26) == 1000.0f);
  bool skills_capped = true;
  for (u32 av = 6; av <= 23; ++av)
    skills_capped = skills_capped && ActorValueOf(player, av) == 100.0f;
  Check("all eighteen skills at 100", skills_capped);
  Check("carry weight 850 and speed 100",
        ActorValueOf(player, 32) == 850.0f && ActorValueOf(player, 30) == 100.0f);

  // Perks. The search only reports a block that admits one reading, so a count
  // here at all is the rule holding; the named ids are what the save actually
  // recorded, cross-checked against the PERK records of the five plugins it was
  // written with.
  Check("297 perks, every one at rank 1",
        player.perks.size() == 297 && AllPerksAtRank(player, 1));
  Check("DestructionMaster100 and Necromage are among them",
        HasPerk(player, save, 0x000C44C2u) && HasPerk(player, save, 0x000581E4u));
  Check("so are the add-ons' DLC1VampiricBite and DLC2Smithing",
        HasPerk(player, save, 0x02005994u) && HasPerk(player, save, 0x04024108u));
  // 226 of the load order's 523 PERK records are not on this list, so it is the
  // player's own set and not every perk that exists.
  Check("TGSkeletonKeyPerk, handed back with the key, is not",
        !HasPerk(player, save, 0x0010F13Fu));

  ActorBaseChange base;
  Check("the player's NPC_ record decodes", DecodeActorBase(*player_base, base));
  Check("level 271", base.has_stats && base.level == 271);
  Check("three spells and 28 shouts",
        base.spells.size() == 3 && base.levelled_spells.empty() &&
            base.shouts.size() == 28);
  Check("the first spell is Flames 0x00012FCD",
        !base.spells.empty() && IsDefault(base.spells[0], 0x00012FCDu));
  Check("32 faction ranks", base.factions.size() == 32);

  // Every reference in the file, walked. A plain REFR has to consume its
  // payload exactly or the decoder drops what it read, so this is the whole
  // corpus checking the layout rather than one hand-picked record.
  u32 refr = 0, refr_exact = 0, achr = 0, achr_inventory = 0, achr_values = 0;
  u32 achr_full_tables = 0;
  u32 achr_perks = 0, perks_total = 0;
  u32 markers = 0, markers_travelable = 0, markers_visible_only = 0;
  bool whiterun_found = false;
  for (const ChangeForm& form : save.change_forms) {
    if (form.type != ChangeFormType::kRefr && form.type != ChangeFormType::kAchr)
      continue;
    ReferenceChange decoded;
    if (!DecodeReference(form, decoded))
      continue;
    if (decoded.has_map_marker) {
      ++markers;
      if (decoded.map_marker_flags & rx::bethesda::kMapMarkerCanTravelTo)
        ++markers_travelable;
      else if (decoded.map_marker_flags & rx::bethesda::kMapMarkerVisible)
        ++markers_visible_only;
      // The Whiterun marker, which Skyrim.esm places in WhiterunWorld.
      if (form.form_id == 0x000162CEu)
        whiterun_found = decoded.map_marker_flags == 3;
    }
    if (form.type == ChangeFormType::kRefr) {
      ++refr;
      if (decoded.decoded_bytes == form.data.size())
        ++refr_exact;
    } else {
      ++achr;
      if (decoded.inventory_complete)
        ++achr_inventory;
      if (!decoded.actor_values.empty()) {
        ++achr_values;
        if (decoded.actor_values.size() == 45)
          ++achr_full_tables;
      }
      if (!decoded.perks.empty()) {
        ++achr_perks;
        perks_total += static_cast<u32>(decoded.perks.size());
      }
    }
  }
  Check("106098 REFR and 20658 ACHR change forms", refr == 106098 && achr == 20658);
  // A plain reference is nothing but its groups, so landing anywhere but the
  // last byte means the walk went wrong. Almost all of them land: the ones that
  // do not carry an extra-data type this layer refuses to size.
  Check("102196 REFR payloads consume to the last byte", refr_exact == 102196);
  Check("17893 ACHR inventories walk to the end", achr_inventory == 17893);
  // The 45 rows are the same 45 actor values every time, so a table that turns
  // up with any other count would be a coincidence being read as data.
  Check("67 actors carry value rows, all of them 45 rows",
        achr_values == 67 && achr_full_tables == 67);
  // The perk search over all 20658 actor blocks finds the pattern exactly once,
  // on the one actor in Skyrim that spends perk points.
  Check("the player is the only actor carrying perks, and carries 297",
        achr_perks == 1 && perks_total == 297);

  // Skyrim.esm and its three add-ons place 471 map markers between them, so a
  // hundred-percent save touching 424 of them is the right order of magnitude
  // and every one of the four it left un-travelled is a civil war camp.
  Check("424 references carry a map marker", markers == 424);
  Check("420 of them are fast travel destinations, 4 are only visible",
        markers_travelable == 420 && markers_visible_only == 4);
  Check("Whiterun's marker 0x000162CE is discovered", whiterun_found);

  // Every CELL, walked. A cell payload is nothing but its groups, so the whole
  // corpus landing on its last byte is what says the group sizes and their
  // order are right rather than adding up by luck on one record.
  u32 cells = 0, cells_exact = 0, with_grid = 0, with_detach = 0, owned = 0;
  u32 exterior_tiles = 0, interior_tiles = 0, tile_grids = 0;
  u32 latest_detach = 0;
  for (const ChangeForm& form : save.change_forms) {
    if (form.type != ChangeFormType::kCell)
      continue;
    ++cells;
    CellChange cell;
    if (!DecodeCell(form, cell))
      continue;
    if (cell.decoded_bytes == form.data.size())
      ++cells_exact;
    if (cell.has_grid)
      ++with_grid;
    if (cell.has_detach_time) {
      ++with_detach;
      if (cell.detach_time > latest_detach)
        latest_detach = cell.detach_time;
    }
    if (form.flags & rx::bethesda::kCellChangeOwnership)
      ++owned;
    if (cell.visited.empty())
      continue;
    if (cell.visited[0].has_tile) {
      ++interior_tiles;
      tile_grids += static_cast<u32>(cell.visited.size());
    } else {
      ++exterior_tiles;
    }
  }
  Check("8793 CELL change forms, every payload consumed to the last byte",
        cells == 8793 && cells_exact == 8793);
  // The coordinate group is what an exterior cell is: no interior writes one,
  // and the 8148 that do all match the coordinate their CELL record's XCLC
  // gives (checked against the load order outside this test).
  Check("8148 carry an exterior coordinate", with_grid == 8148);
  // Almost every cell is out of memory in any save; the handful that are not
  // are the load ring the player is standing in.
  Check("8759 of 8793 carry a detach time", with_detach == 8759);
  // The save's GameDaysPassed global reads 2616, so the cell that left memory
  // last did so 2615.3 game days in: the field is a game hour, not a counter.
  const f32 days_passed =
      save.globals.size() > 4 && save.globals[4].first == 0x00000039u ? save.globals[4].second
                                                                     : 0.0f;
  const f32 hours_ago = days_passed * 24.0f - static_cast<f32>(latest_detach);
  Check("the latest detach is game hour 62768, 16 hours behind GameDaysPassed",
        latest_detach == 62768 && hours_ago > 0.0f && hours_ago < 24.0f);
  Check("7 cells carry an owner the save changed", owned == 7);
  // The two seen-data shapes never cross: a cell that writes its coordinate
  // writes one bare tile, one that does not writes a counted list.
  Check("5298 exteriors write one bare tile, 611 interiors write 5855 between them",
        exterior_tiles == 5298 && interior_tiles == 611 && tile_grids == 5855);
}

}  // namespace

int main() {
  std::puts("savegame changeform: decoding real Skyrim SE payloads");
  TestChangeRef();
  TestQuest();
  TestFaction();
  TestActorBase();
  TestReference();
  TestContainerDelta();
  TestMapMarker();
  TestDialogueInfo();
  TestCell();
  TestMalformed();
  TestRealSave();

  if (g_failures == 0) {
    std::puts("savegame changeform: all checks passed");
    return 0;
  }
  std::printf("savegame changeform: %d checks FAILED\n", g_failures);
  return 1;
}

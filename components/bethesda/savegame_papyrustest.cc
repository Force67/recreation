// savegame_papyrustest: the Papyrus heap, against the real save's bytes.
//
// Every number asserted here was measured off the reference save first (a
// standalone python walk of the same bytes), so a layout that drifts shows up
// as a count that no longer matches rather than as a plausible-looking parse.
// The checks that matter are the ones nothing but a correct layout can satisfy:
// each data block repeating its own header row's id, the flattened member list
// of every script matching its instance's value count, and every object value
// in the heap landing on an object that is actually in it.
//
// Skipped when the save is not on this machine; point RX_SAVEGAME_TEST_FILE at
// one to run it elsewhere.

#include <base/containers/unordered_map.h>
#include <base/containers/unordered_set.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstdlib>

#include "components/bethesda/savegame.h"
#include "components/bethesda/savegame_apply.h"
#include "components/bethesda/savegame_papyrus.h"
#include "core/types.h"

namespace {

using rx::f32;
using rx::u32;
using rx::u64;
using rx::u8;
using rx::bethesda::GlobalFormId;
using rx::bethesda::PapyrusInstance;
using rx::bethesda::PapyrusRestore;
using rx::bethesda::PapyrusRestoreStats;
using rx::bethesda::PapyrusValueType;
using rx::bethesda::SaveFile;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

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

// An empty table, a truncated one and a table whose string count cannot fit:
// all three have to be refused rather than half read.
void TestRejects() {
  std::puts("malformed tables");
  rx::bethesda::PapyrusHeap heap;
  base::Vector<u32> no_forms;
  Check("empty table refused", !rx::bethesda::ReadPapyrusHeap(rx::ByteSpan(), no_forms, &heap));

  const u8 short_table[] = {6, 0, 0xff, 0xff, 0xff, 0x7f};  // version, absurd string count
  Check("impossible string count refused",
        !rx::bethesda::ReadPapyrusHeap(rx::ByteSpan(short_table, sizeof(short_table)), no_forms,
                                       &heap));
  Check("nothing was kept", !heap.present && heap.instances.empty());
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
  const rx::bethesda::PapyrusHeap& heap = save.papyrus;
  Check("the heap is there", heap.present);
  if (!heap.present)
    return;

  Check("header version 6", heap.version == 6);
  Check("13183418 byte table", heap.table_bytes == 13183418);
  Check("29385 strings", heap.strings.size() == 29385);
  Check("5283 script definitions", heap.script_count == 5283);
  Check("125195 script instances", heap.instances.size() == 125195);
  Check("4206 heap references", heap.reference_count == 4206);
  Check("344 arrays", heap.arrays.size() == 344);
  Check("21 active scripts", heap.active_script_count == 21);
  Check("840117 member variables", heap.variables.size() == 840117);
  // Every script the instances name is in the definition table, so no
  // instance's variables were left without names.
  Check("every variable is named", heap.unnamed_variables == 0);
  // The strongest single number here: the walk lands exactly on the byte the
  // arrays end at, leaving only the 21 active scripts' own frames, which are
  // deliberately not decoded.
  Check("the walk consumed 13161755 bytes", heap.consumed_bytes == 13161755);
  Check("21663 bytes of stacks are left alone",
        heap.table_bytes - heap.consumed_bytes == 21663);

  // The first row, whole. wealiasscript is a world-encounter alias script: 15
  // members, the first an Activator reference and the twelfth an Int.
  const PapyrusInstance& first = heap.instances[0];
  Check("first instance script", heap.strings[first.script] == "wealiasscript");
  Check("first instance is on a quest alias", first.alias_id == 61 && first.kind == 0);
  Check("first instance quest form", first.form_id == 0x000c1a1f);
  Check("first instance is in the default state", first.state == 0);
  Check("first instance has 15 variables", first.variable_count == 15);
  if (first.variable_count == 15) {
    const rx::bethesda::PapyrusVariable* v = heap.variables.data() + first.first_variable;
    Check("first member name", heap.strings[v[0].name] == "::DefaultAshPile1_var");
    Check("first member is an Activator ref",
          v[0].value.type == PapyrusValueType::kRef &&
              heap.strings[v[0].value.name] == "Activator" && v[0].value.data == 0);
    Check("tenth member is a Faction ref",
          v[9].value.type == PapyrusValueType::kRef && heap.strings[v[9].value.name] == "Faction");
    Check("twelfth member is int -1",
          v[11].value.type == PapyrusValueType::kInt && v[11].value.AsInt() == -1);
  }

  // Which is which is measured, not assumed: kind 1 and 2 are the two counts
  // that fell out of the u16 histogram, and every kind 2 script descends from
  // ActiveMagicEffect.
  u32 by_kind[3] = {0, 0, 0};
  u32 alias_rows = 0;
  u32 non_default_states = 0;
  for (const PapyrusInstance& inst : heap.instances) {
    if (inst.kind < 3)
      ++by_kind[inst.kind];
    if (inst.alias_id != 0xffff)
      ++alias_rows;
    if (inst.state != 0)
      ++non_default_states;
  }
  Check("122673 plain instances", by_kind[0] == 122673);
  Check("2341 of kind 1", by_kind[1] == 2341);
  Check("181 active magic effects", by_kind[2] == 181);
  Check("14914 rows carry an alias index", alias_rows == 14914);
  Check("32496 instances are in a named state", non_default_states == 32496);

  // The closing evidence: every object value in the heap lands on something
  // that is in the heap. A wrong value width would leave dangling ids.
  base::UnorderedSet<u64> instance_ids;
  for (const PapyrusInstance& inst : heap.instances)
    instance_ids.insert(inst.id);
  base::UnorderedSet<u64> array_ids;
  for (const rx::bethesda::PapyrusArray& array : heap.arrays)
    array_ids.insert(array.id);

  u32 refs_none = 0, refs_instance = 0, refs_other = 0;
  u32 array_values = 0, array_dangling = 0;
  for (const rx::bethesda::PapyrusVariable& var : heap.variables) {
    if (var.value.type == PapyrusValueType::kRef) {
      if (var.value.data == 0)
        ++refs_none;
      else if (instance_ids.contains(var.value.data))
        ++refs_instance;
      else
        ++refs_other;
    } else if (var.value.IsArray() && var.value.data != 0) {
      ++array_values;
      if (!array_ids.contains(var.value.data))
        ++array_dangling;
    }
  }
  Check("193330 object values name a script instance", refs_instance == 193330);
  Check("111163 object values are None", refs_none == 111163);
  Check("4147 name a formless heap reference", refs_other == 4147);
  Check("341 array values, none dangling", array_values == 341 && array_dangling == 0);

  // Kept across the move below, which empties save.papyrus into the index.
  const u64 first_id = first.id;

  // The restore index, against the save's own load order (every plugin present,
  // which is the case a real run of the same game hits).
  rx::bethesda::FormRemap remap;
  remap.Build(save, [&save](const base::String& name) -> rx::u16 {
    for (size_t i = 0; i < save.plugins.size(); ++i)
      if (save.plugins[i] == name)
        return static_cast<rx::u16>(i);
    return 0xffff;
  });

  PapyrusRestore restore;
  PapyrusRestoreStats stats;
  restore.Build(base::move(save.papyrus), remap, &stats);
  Check("every instance was seen", stats.instances == 125195);
  Check("the magic effects were skipped", stats.magic_effect == 181);
  Check("the kind 1 rows were skipped", stats.other_kind == 2341);
  Check("every row is either indexed or accounted for",
        stats.indexed + stats.magic_effect + stats.other_kind + stats.no_form +
                stats.missing_plugin ==
            125195);
  Check("20651 sit on references the save spawned", stats.created_form == 20651);
  Check("122671 instances are addressable", stats.indexed == 122671);
  Check("two rows resolve to no form at all", stats.no_form == 2);
  std::printf("    indexed %u of %u instances (%u variables), %u of them on spawned references\n",
              stats.indexed, stats.instances, stats.variables, stats.created_form);

  // The alias instance from the first row, found the way the engine finds it.
  const PapyrusInstance* found = restore.Find(GlobalFormId{0, 0x000c1a1f}, 61, "WEAliasScript");
  Check("the first alias instance is found by form, alias and script (case insensitively)",
        found != nullptr);
  if (found != nullptr)
    Check("and it is the same row", found->id == first_id && found->variable_count == 15);
  Check("a script that is not on that alias is not found",
        restore.Find(GlobalFormId{0, 0x000c1a1f}, 61, "PressurePlate") == nullptr);
  Check("the same form without the alias index is a different key",
        restore.Find(GlobalFormId{0, 0x000c1a1f}, 0xffff, "WEAliasScript") == nullptr);

  // An object value resolves to the form its target instance sits on.
  u32 resolved = 0, unresolved = 0;
  for (const rx::bethesda::PapyrusVariable& var : restore.heap().variables) {
    if (var.value.type != PapyrusValueType::kRef || var.value.data == 0)
      continue;
    if (restore.Resolve(var.value.data).valid)
      ++resolved;
    else
      ++unresolved;
  }
  Check("object values resolve to a form", resolved > 150000);
  std::printf("    %u object values resolve to a form, %u do not\n", resolved, unresolved);
}

}  // namespace

int main() {
  std::puts("savegame_papyrustest");
  TestRejects();
  TestRealSave();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}

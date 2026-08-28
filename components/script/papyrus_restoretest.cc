// papyrus_restoretest: a savegame's Papyrus heap onto a live VM.
//
// The heap here is synthesised byte for byte in the layout savegame_papyrus.cc
// measured off a real Skyrim SE save (savegame_papyrustest asserts that layout
// against the real file; this asserts what the engine does with it). Building
// the bytes rather than reading a save is what makes the value conversions,
// the alias handle key and the OnInit suppression testable with no game data.

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstring>

#include "components/bethesda/savegame_apply.h"
#include "components/bethesda/savegame_papyrus.h"
#include "components/script/papyrus/alias_handle.h"
#include "components/script/papyrus/pex.h"
#include "components/script/papyrus/vm.h"
#include "components/script/papyrus_restore.h"

namespace {

using rx::f32;
using rx::i32;
using rx::u16;
using rx::u32;
using rx::u8;
using rx::bethesda::GlobalFormId;
using rx::script::PapyrusRestorer;
using rx::script::papyrus::ObjectRef;
using rx::script::papyrus::PexFile;
using rx::script::papyrus::Value;
using rx::script::papyrus::ValueType;
using rx::script::papyrus::VirtualMachine;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

// --- the file side: a table 1001 built to the measured layout ---------------

struct HeapWriter {
  base::Vector<u8> bytes;
  base::Vector<base::String> strings;

  void U8(u8 v) { bytes.push_back(v); }
  void U16(u16 v) {
    bytes.push_back(u8(v));
    bytes.push_back(u8(v >> 8));
  }
  void U32(u32 v) {
    for (int i = 0; i < 4; ++i)
      bytes.push_back(u8(v >> (8 * i)));
  }
  void U64(rx::u64 v) {
    for (int i = 0; i < 8; ++i)
      bytes.push_back(u8(v >> (8 * i)));
  }
  void F32(f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    U32(bits);
  }
  // A form id the save resolved against the first master, which is RefID kind 1
  // (three big endian bytes with the kind in the top two).
  void MasterRef(u32 form_id) {
    bytes.push_back(u8(0x40 | ((form_id >> 16) & 0x3f)));
    bytes.push_back(u8(form_id >> 8));
    bytes.push_back(u8(form_id));
  }

  u32 Str(const base::String& value) {
    for (size_t i = 0; i < strings.size(); ++i)
      if (strings[i] == value)
        return static_cast<u32>(i);
    strings.push_back(value);
    return static_cast<u32>(strings.size() - 1);
  }
};

// One script instance's worth of the file: a definition, a header row and a
// data block. Two scripts, so the alias key and the plain form key both appear.
base::Vector<u8> BuildTable() {
  HeapWriter w;
  // The strings have to exist before the table can be written, so the indices
  // are taken first and the header is emitted around them below.
  const u32 s_empty = w.Str("");
  const u32 s_script = w.Str("DoorScript");
  const u32 s_alias_script = w.Str("HouseAliasScript");
  const u32 s_object_reference = w.Str("ObjectReference");
  const u32 s_referencealias = w.Str("ReferenceAlias");
  const u32 s_open = w.Str("::Open_var");
  const u32 s_bool = w.Str("Bool");
  const u32 s_count = w.Str("::Count_var");
  const u32 s_int = w.Str("Int");
  const u32 s_owner = w.Str("::Owner_var");
  const u32 s_name = w.Str("::Name_var");
  const u32 s_string = w.Str("String");
  const u32 s_price = w.Str("::Price_var");
  const u32 s_float = w.Str("Float");
  const u32 s_rooms = w.Str("::Rooms_var");
  const u32 s_locked = w.Str("Locked");
  const u32 s_bought = w.Str("::Bought_var");

  HeapWriter out;
  out.strings = w.strings;
  out.U16(6);
  out.U32(static_cast<u32>(out.strings.size()));
  for (const base::String& s : out.strings) {
    out.U16(static_cast<u16>(s.size()));
    for (size_t i = 0; i < s.size(); ++i)
      out.U8(static_cast<u8>(s[i]));
  }

  // Script definitions. DoorScript declares five members of its own;
  // HouseAliasScript declares one and inherits DoorScript's five, which is what
  // makes the parent-chain-first flattening observable.
  out.U32(2);
  out.U32(s_script);
  out.U32(s_empty);
  out.U32(5);
  out.U32(s_open);
  out.U32(s_bool);
  out.U32(s_count);
  out.U32(s_int);
  out.U32(s_owner);
  out.U32(s_object_reference);
  out.U32(s_name);
  out.U32(s_string);
  out.U32(s_price);
  out.U32(s_float);

  out.U32(s_alias_script);
  out.U32(s_script);  // parent
  out.U32(1);
  out.U32(s_rooms);
  out.U32(s_int);

  // Instance header rows, 20 bytes each.
  constexpr rx::u64 kDoorId = 0x0000021200001000ull;
  constexpr rx::u64 kAliasId = 0x0000021200002000ull;
  out.U32(2);
  out.U64(kDoorId);
  out.U32(s_script);
  out.U16(0);       // kind
  out.U16(0xffff);  // no alias
  out.MasterRef(0x0001a2b3);
  out.U8(1);

  out.U64(kAliasId);
  out.U32(s_alias_script);
  out.U16(0);
  out.U16(7);  // alias index
  out.MasterRef(0x000c1a1f);
  out.U8(1);

  out.U32(0);  // no heap references

  // One int array, referenced from the alias instance's inherited Count member.
  constexpr rx::u64 kArrayId = 0x0000021200003000ull;
  out.U32(1);
  out.U64(kArrayId);
  out.U8(static_cast<u8>(rx::bethesda::PapyrusValueType::kInt));
  out.U32(3);

  out.U32(0);  // runtime counter
  out.U32(0);  // no active scripts

  // Data blocks, in the same order as the header rows and repeating their ids.
  out.U64(kDoorId);
  out.U8(0x0b);
  out.U32(s_locked);  // current state
  out.U32(0);
  out.U32(5);
  out.U8(5);  // Open = true
  out.U32(1);
  out.U8(3);  // Count = -4
  out.U32(static_cast<u32>(-4));
  out.U8(1);  // Owner = the alias instance
  out.U32(s_object_reference);
  out.U64(kAliasId);
  out.U8(2);  // Name
  out.U32(s_referencealias);
  out.U8(4);  // Price = 2.5
  out.F32(2.5f);

  out.U64(kAliasId);
  out.U8(0x0b);
  out.U32(s_empty);  // default state
  out.U32(0);
  out.U32(6);
  out.U8(5);  // inherited Open = false
  out.U32(0);
  out.U8(13);  // inherited Count = the int array
  out.U64(kArrayId);
  out.U8(1);  // inherited Owner = None
  out.U32(s_object_reference);
  out.U64(0);
  out.U8(2);  // inherited Name
  out.U32(s_bought);
  out.U8(4);  // inherited Price
  out.F32(0.0f);
  out.U8(3);  // its own Rooms = 3
  out.U32(3);

  out.U64(kArrayId);
  out.U8(3);
  out.U32(11);
  out.U8(3);
  out.U32(22);
  out.U8(3);
  out.U32(33);
  return base::move(out.bytes);
}

// --- the VM side ------------------------------------------------------------

struct PexBuilder {
  PexFile pex;
  base::UnorderedMap<base::String, rx::script::papyrus::StringIndex> strings;

  rx::script::papyrus::StringIndex String(const base::String& value) {
    if (auto* it = strings.find(value))
      return *it;
    const auto index = static_cast<rx::script::papyrus::StringIndex>(pex.string_table.size());
    pex.string_table.push_back(value);
    strings.emplace(value, index);
    return index;
  }
};

PexFile MakeScript(const base::String& name,
                   const base::String& parent,
                   std::initializer_list<const char*> members) {
  PexBuilder builder;
  rx::script::papyrus::Object object;
  object.name = builder.String(name);
  object.parent_class = builder.String(parent);
  for (const char* member : members)
    object.variables.push_back({builder.String(member), builder.String("None"), 0, {}});
  rx::script::papyrus::State state;
  state.name = builder.String("");
  object.states.push_back(base::move(state));
  builder.pex.objects.push_back(base::move(object));
  return base::move(builder.pex);
}

void TestRestore() {
  rx::bethesda::PapyrusHeap heap;
  base::Vector<u32> form_ids;
  const base::Vector<u8> table = BuildTable();
  Check("the synthetic table parses",
        rx::bethesda::ReadPapyrusHeap(rx::ByteSpan(table.data(), table.size()), form_ids, &heap));
  Check("it consumed every byte", heap.consumed_bytes == table.size());
  Check("two instances", heap.instances.size() == 2);
  Check("one array", heap.arrays.size() == 1);
  Check("eleven variables (5 + 6)", heap.variables.size() == 11);
  Check("no variable was left unnamed", heap.unnamed_variables == 0);
  // The inherited members come first: the alias script's own Rooms is last.
  if (heap.instances.size() == 2 && heap.variables.size() == 11)
    Check("the parent's members are ahead of the script's own",
          heap.strings[heap.variables[10].name] == "::Rooms_var");

  // A remap that keeps the one plugin the synthetic RefIDs name.
  rx::bethesda::SaveFile save;
  save.plugins.push_back("Skyrim.esm");
  rx::bethesda::FormRemap remap;
  remap.Build(save, [](const base::String& name) -> u16 { return name == "Skyrim.esm" ? 0 : 0xffff; });

  PapyrusRestorer restorer;
  restorer.Build(base::move(heap), remap);

  VirtualMachine vm(nullptr);
  vm.AddScript(MakeScript("DoorScript", "",
                          {"::Open_var", "::Count_var", "::Owner_var", "::Name_var",
                           "::Price_var"}));
  vm.AddScript(MakeScript("HouseAliasScript", "DoorScript", {"::Rooms_var"}));

  const rx::u64 door = GlobalFormId{0, 0x0001a2b3}.packed();
  const rx::u64 house =
      rx::script::papyrus::EncodeAliasHandle(GlobalFormId{0, 0x000c1a1f}.packed(), 7);
  vm.CreateInstanceWithHandle("DoorScript", door);
  vm.CreateInstanceWithHandle("HouseAliasScript", house);

  Check("the door instance is claimed", restorer.Apply(vm, ObjectRef{door}, "DoorScript"));
  Check("a script the save does not have on that form is not claimed",
        !restorer.Apply(vm, ObjectRef{door}, "HouseAliasScript"));
  Check("the alias instance is claimed, keyed by quest and alias index",
        restorer.Apply(vm, ObjectRef{house}, "HouseAliasScript"));

  const Value* open = vm.MemberVar(ObjectRef{door}, "::Open_var");
  Check("bool restored", open && open->type() == ValueType::kBool && open->as_bool());
  const Value* count = vm.MemberVar(ObjectRef{door}, "::Count_var");
  Check("negative int restored", count && count->as_int() == -4);
  const Value* price = vm.MemberVar(ObjectRef{door}, "::Price_var");
  Check("float restored", price && price->as_float() == 2.5f);
  const Value* name = vm.MemberVar(ObjectRef{door}, "::Name_var");
  Check("string restored", name && name->as_string() == "ReferenceAlias");
  // The saved object value named the alias instance, so it comes back as that
  // instance's engine handle, not as the raw heap id.
  const Value* owner = vm.MemberVar(ObjectRef{door}, "::Owner_var");
  Check("object value resolved to the alias handle",
        owner && owner->type() == ValueType::kObject && owner->as_object().handle == house);

  Check("the state came back", vm.CurrentState(ObjectRef{door}) == "Locked");
  Check("a default state stays default", vm.CurrentState(ObjectRef{house}).empty());

  const Value* rooms = vm.MemberVar(ObjectRef{house}, "::Rooms_var");
  Check("the script's own member restored", rooms && rooms->as_int() == 3);
  const Value* array_member = vm.MemberVar(ObjectRef{house}, "::Count_var");
  Check("array member restored", array_member && array_member->type() == ValueType::kArray);
  if (array_member && array_member->type() == ValueType::kArray) {
    const rx::script::papyrus::ArrayRef a = array_member->as_array();
    Check("array length", vm.ArrayLength(a) == 3);
    Check("array contents",
          vm.ArrayGet(a, 0).as_int() == 11 && vm.ArrayGet(a, 1).as_int() == 22 &&
              vm.ArrayGet(a, 2).as_int() == 33);
  }
  Check("two instances restored", restorer.restored_instances() == 2);
  // All eleven: the door's five and the alias script's inherited five plus its
  // own Rooms, every one of them a member its script declares.
  //
  // This read 10 until the flattener stopped feeding push_back an element of the
  // vector it was pushing into. Growing frees the old block before constructing
  // from that reference, so the inherited ::Name_var came back as a fragment of
  // a freed heap pointer, landed out of range of the string table, and was
  // dropped as nameless. The count was the corruption, not a rule.
  Check("all eleven members restored", restorer.restored_members() == 11);
}

// A member the save has and the script here does not is counted and dropped,
// never invented: a member no compiled code reads would be dead weight that
// only masks the mismatch.
void TestUndeclaredMember() {
  rx::bethesda::PapyrusHeap heap;
  base::Vector<u32> form_ids;
  const base::Vector<u8> table = BuildTable();
  rx::bethesda::ReadPapyrusHeap(rx::ByteSpan(table.data(), table.size()), form_ids, &heap);

  rx::bethesda::SaveFile save;
  save.plugins.push_back("Skyrim.esm");
  rx::bethesda::FormRemap remap;
  remap.Build(save, [](const base::String&) -> u16 { return 0; });
  PapyrusRestorer restorer;
  restorer.Build(base::move(heap), remap);

  VirtualMachine vm(nullptr);
  // The same script, rebuilt with only two of its five members.
  vm.AddScript(MakeScript("DoorScript", "", {"::Open_var", "::Price_var"}));
  const rx::u64 door = GlobalFormId{0, 0x0001a2b3}.packed();
  vm.CreateInstanceWithHandle("DoorScript", door);
  Check("still claimed", restorer.Apply(vm, ObjectRef{door}, "DoorScript"));
  Check("only the declared members landed", restorer.restored_members() == 2);
  Check("nothing was invented", vm.MemberVar(ObjectRef{door}, "::Count_var") == nullptr);
}

// A save the load order cannot carry: the plugin the instances name is gone, so
// every one of them is refused rather than landing on whatever now sits at that
// load order slot.
void TestMissingPlugin() {
  rx::bethesda::PapyrusHeap heap;
  base::Vector<u32> form_ids;
  const base::Vector<u8> table = BuildTable();
  rx::bethesda::ReadPapyrusHeap(rx::ByteSpan(table.data(), table.size()), form_ids, &heap);

  rx::bethesda::SaveFile save;
  save.plugins.push_back("Gone.esp");
  rx::bethesda::FormRemap remap;
  remap.Build(save, [](const base::String&) -> u16 { return 0xffff; });
  PapyrusRestorer restorer;
  restorer.Build(base::move(heap), remap);
  Check("nothing was indexed", restorer.stats().indexed == 0);
  Check("both instances were refused", restorer.stats().missing_plugin == 2);

  VirtualMachine vm(nullptr);
  vm.AddScript(MakeScript("DoorScript", "", {"::Open_var"}));
  const rx::u64 door = GlobalFormId{0, 0x0001a2b3}.packed();
  vm.CreateInstanceWithHandle("DoorScript", door);
  Check("and nothing is claimed", !restorer.Apply(vm, ObjectRef{door}, "DoorScript"));
}

}  // namespace

int main() {
  std::puts("papyrus_restoretest");
  TestRestore();
  TestUndeclaredMember();
  TestMissingPlugin();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}

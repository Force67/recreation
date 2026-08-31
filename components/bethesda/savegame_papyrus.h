#ifndef COMPONENTS_BETHESDA_SAVEGAME_PAPYRUS_H
#define COMPONENTS_BETHESDA_SAVEGAME_PAPYRUS_H

// The Papyrus heap out of a savegame: every script instance the game had live,
// the state it was sitting in, and the value of every one of its member
// variables. On the reference save that is 40% of the whole decompressed body
// (13,183,418 of 32,653,551 bytes) and it is the single biggest thing a save
// carries. Without it a quest can show the right journal stage while its script
// believes something else entirely, and scripted doors, followers, houses,
// marriage and DLC systems reset or repeat.
//
// It lives in global data table 1001. Two layers, like the rest of the reader:
//
//   PapyrusHeap    is the file's own shape. Names are string table indices and
//                  object values are the save's own heap ids, because that is
//                  what is written down. Knows nothing about a load order.
//   PapyrusRestore turns that into this run's terms: form ids remapped, heap
//                  ids resolved to the form (or quest alias) they name, and an
//                  index from an engine handle to what the save held for it.
//
// EVERY LAYOUT BELOW WAS MEASURED against the reference save, not taken from a
// wiki. Where a field is still unexplained it says so rather than guessing.

#include <base/containers/pair.h>
#include <base/containers/span.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "core/types.h"

namespace rx::bethesda {

class FormRemap;

// A Papyrus value as the heap writes it. The numbers are the file's own type
// tags: measured across 840,117 variables, only these eleven occur, and reading
// each at the width below walks all 125,195 instances to the byte.
enum class PapyrusValueType : u8 {
  kNone = 0,
  kRef = 1,     // declared class name + the heap id of the object
  kString = 2,
  kInt = 3,
  kFloat = 4,
  kBool = 5,
  kRefArray = 11,  // declared element class + the heap id of the array
  kStringArray = 12,
  kIntArray = 13,
  kFloatArray = 14,
  kBoolArray = 15,
};

struct PapyrusValue {
  PapyrusValueType type = PapyrusValueType::kNone;
  // kString: the string table index of the text. kRef/kRefArray: the index of
  // the class name the heap stores beside the handle. Unused otherwise.
  u32 name = 0;
  // kInt/kBool: the value. kFloat: its bits. kRef and the array kinds: the heap
  // id of the object or array it points at, 0 for None.
  u64 data = 0;

  i32 AsInt() const { return static_cast<i32>(static_cast<u32>(data)); }
  bool AsBool() const { return data != 0; }
  f32 AsFloat() const;
  bool IsArray() const { return type >= PapyrusValueType::kRefArray; }
};

// One member variable of an instance. `name` is the name the script declares it
// under, which is what makes this restorable at all: our VM stores members by
// name too. Auto-properties appear under their backing name (`::Foo_var`), the
// same spelling the compiled .pex uses.
struct PapyrusVariable {
  u32 name = 0;  // string table index; 0 when the script's members are unknown
  PapyrusValue value;
};

// A script instance. Rows are a fixed 20 bytes in the header table and their
// values follow in a separate block later in the same order, keyed by the same
// id; the walk checks that pairing on every one of the 125,195 rows.
struct PapyrusInstance {
  u64 id = 0;       // the save process's own heap address, what a kRef points at
  u32 script = 0;   // string table index of the script type name
  u32 form_id = 0;  // RefID resolved through the save's form id map
  // The quest alias this instance is attached to, 0xffff when it is not one. An
  // alias instance's form_id is the QUEST, not the reference filling the alias:
  // 10,460 of the 14,914 rows carrying an alias index resolve to a form the save
  // also holds a QUST change form for, and every one of the 12,392 whose script
  // descends from Alias carries one.
  u16 alias_id = 0xffff;
  // 0 = an instance on a form or a quest alias, 1 = unexplained (2,341 rows,
  // all scripts descending from Form, all carrying the u16 above), 2 = an
  // active magic effect (181 rows, every script descending from
  // ActiveMagicEffect). Only kind 0 is something the engine can address.
  u8 kind = 0;
  u32 state = 0;  // string table index of the current state, 0 = the default one
  u32 first_variable = 0;
  u32 variable_count = 0;
};

// A VM-owned array. Its values sit in PapyrusHeap::array_values.
struct PapyrusArray {
  u64 id = 0;
  u32 element_class = 0;  // string table index, only written for a ref array
  u8 element_type = 0;    // a PapyrusValueType scalar tag
  u32 first_value = 0;
  u32 value_count = 0;
};

// The heap as the file has it.
struct PapyrusHeap {
  bool present = false;
  u16 version = 0;  // 6 on Skyrim SE

  base::Vector<base::String> strings;
  base::Vector<PapyrusInstance> instances;
  base::Vector<PapyrusVariable> variables;  // instances index into this
  base::Vector<PapyrusArray> arrays;
  base::Vector<PapyrusValue> array_values;  // arrays index into this

  // Walked past, not decoded.
  //
  // References are heap objects with a type name and no form: 4,206 of them,
  // and their data blocks carry no variables at all, so nothing in the file
  // says which form one stands for. 4,147 member variables point at one; they
  // are counted and dropped.
  u32 reference_count = 0;
  // Active and suspended stacks. Their frames name Bethesda's own VM's
  // instruction positions and stack layout, which our VM shares nothing with,
  // so resuming an in-flight script is out of scope: the heap is read up to
  // them and they are left alone. Losing an in-flight script costs far less
  // than a wrong parse of everything before it.
  u32 active_script_count = 0;
  u32 script_count = 0;  // script definitions the file declares
  u32 unnamed_variables = 0;  // values whose declaring script was not in the file

  size_t table_bytes = 0;  // what global data table 1001 declared
  size_t consumed_bytes = 0;  // what the walk actually read, for the reader's own check
};

// Reads global data table 1001. `form_ids` is the save's form id map, which the
// RefIDs inside resolve through. Returns false and leaves `out` empty on a
// layout that does not walk; a partial parse is never handed back, because half
// a heap restored onto live scripts is worse than none.
bool ReadPapyrusHeap(ByteSpan table, const base::Vector<u32>& form_ids, PapyrusHeap* out);

// Which instance a saved heap id names, in this run's terms.
struct PapyrusTarget {
  GlobalFormId form;
  u32 alias_id = 0xffff;  // set when the instance is on a quest alias
  bool valid = false;
};

struct PapyrusRestoreStats {
  u32 instances = 0;             // rows in the heap
  u32 indexed = 0;               // rows this run can address
  u32 missing_plugin = 0;        // the form's plugin is not loaded
  // Of the indexed rows, the ones on a reference the save spawned. They are
  // keyed under the created-reference slot, so only the spawns the streamer
  // actually places will ever ask for them.
  u32 created_form = 0;
  u32 magic_effect = 0;          // kind 2, an active magic effect
  u32 other_kind = 0;            // kind 1, unexplained
  u32 no_form = 0;               // RefID resolved to nothing
  u32 variables = 0;             // variables behind the indexed rows
  u32 arrays = 0;
  u32 non_default_states = 0;
  u32 refs_to_references = 0;    // object values naming a formless heap object
};

// The heap in this run's terms, and the index the engine looks into whenever a
// script attaches.
//
// It owns the heap rather than borrowing it: a reference's scripts only attach
// when its cell streams in, which is minutes after the save file itself is let
// go, so this has to outlive everything else the save produced.
class PapyrusRestore {
 public:
  void Build(PapyrusHeap heap, const FormRemap& remap, PapyrusRestoreStats* stats);

  bool empty() const { return index_.empty(); }
  size_t size() const { return index_.size(); }

  // What the save held for one form (or quest alias) and script type, or
  // nullptr. `script` is matched case insensitively, the way Papyrus names are
  // matched everywhere else.
  const PapyrusInstance* Find(GlobalFormId form, u32 alias_id, base::StringRef script) const;

  const base::String& Str(u32 index) const;
  base::Span<const PapyrusVariable> VariablesOf(const PapyrusInstance& instance) const;

  // The array a kRef*Array value points at, or nullptr.
  const PapyrusArray* FindArray(u64 heap_id) const;
  base::Span<const PapyrusValue> ValuesOf(const PapyrusArray& array) const;

  // The form (or quest alias) a kRef value's heap id names. Invalid for 0, for
  // a formless heap reference, and for an id whose form this run cannot address.
  PapyrusTarget Resolve(u64 heap_id) const;

  const PapyrusHeap& heap() const { return heap_; }

 private:
  // Sorted (key, instance) pairs rather than a map of vectors: a form carries a
  // handful of scripts and there are six figures of rows, so one sorted array
  // and a binary search costs 12 bytes a row instead of a container each.
  static u64 KeyOf(GlobalFormId form, u32 alias_id);

  PapyrusHeap heap_;
  base::Vector<base::Pair<u64, u32>> index_;      // (form/alias key, instance)
  base::Vector<base::Pair<u64, u32>> by_heap_id_;  // (heap id, instance)
  base::Vector<base::Pair<u64, u32>> arrays_;      // (heap id, array)
  base::Vector<PapyrusTarget> targets_;            // per instance, this run's form
};

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_PAPYRUS_H

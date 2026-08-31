#include "components/bethesda/savegame_papyrus.h"

#include <base/algorithm.h>
#include <base/containers/unordered_map.h>
#include <base/memory/move.h>

#include <cstring>

#include "components/bethesda/savegame_apply.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

// What table 1001 looks like on the reference save, all of it read off the
// bytes rather than off a wiki (FallrimTools' own convention for the first of
// these is wrong for this file, see the string index note below):
//
//   u16                     header version, 6
//   u32 + that many wstring string table, 29,385 entries, 742,167 bytes
//   u32 + rows              script definitions, 5,283
//   u32 + 20 byte rows      script instances, 125,195
//   u32 + 12 byte rows      heap references, 4,206
//   u32 + rows              array descriptions, 344
//   u32                     a runtime counter
//   u32 + 5 byte rows       active scripts, 21
//   per instance            its state and its variables' values
//   per reference           the same shape, always with zero variables
//   per array               its elements
//   the stacks              not read (see the header)
//
// The three walks that carry the payload each end exactly where the next table
// begins and each data block repeats its own header row's id, which is what
// says the sizes below are right rather than merely plausible.

// A string table index is a u32 here. Measured: read as a u16 every second
// value comes back zero, which is what a little endian u32 looks like through a
// u16 window, and the script names it yields stop being real.
constexpr u32 kNoAlias = 0xffff;

// Set on a script instance's data block when an extra u32 sits between the
// unexplained field and the variable count. 4 of 125,195 rows have it.
constexpr u8 kDataFlagExtraField = 0x04;

// No save has a script with more members than this, so a count past it is a
// desynced walk rather than something to allocate for.
constexpr u32 kMaxMembers = 1u << 16;

char Fold(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

base::String Lower(const base::String& s) {
  base::String out = s;
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = Fold(out[i]);
  return out;
}

// Papyrus names are case insensitive everywhere else in the VM, so a script
// name out of the save matches one out of a .pex the same way.
bool IEquals(const base::String& a, base::StringRef b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Fold(a[i]) != Fold(b[i]))
      return false;
  return true;
}

class Reader {
 public:
  explicit Reader(ByteSpan bytes)
      : begin_(bytes.data()), p_(bytes.data()), end_(bytes.data() + bytes.size()) {}

  bool ok() const { return ok_; }
  size_t offset() const { return static_cast<size_t>(p_ - begin_); }
  size_t remaining() const { return ok_ ? static_cast<size_t>(end_ - p_) : 0; }

  bool Need(size_t n) {
    if (!ok_ || static_cast<size_t>(end_ - p_) < n)
      ok_ = false;
    return ok_;
  }
  u8 U8() {
    if (!Need(1))
      return 0;
    return *p_++;
  }
  u16 U16() {
    if (!Need(2))
      return 0;
    u16 v;
    std::memcpy(&v, p_, 2);
    p_ += 2;
    return v;
  }
  u32 U32() {
    if (!Need(4))
      return 0;
    u32 v;
    std::memcpy(&v, p_, 4);
    p_ += 4;
    return v;
  }
  u64 U64() {
    if (!Need(8))
      return 0;
    u64 v;
    std::memcpy(&v, p_, 8);
    p_ += 8;
    return v;
  }
  base::String WString() {
    const u16 len = U16();
    if (!Need(len))
      return {};
    base::String s(reinterpret_cast<const char*>(p_), len);
    p_ += len;
    return s;
  }
  // Three big endian bytes with a two bit kind in the top of the first, the
  // same RefID the rest of the save uses.
  u32 RefId(const base::Vector<u32>& form_ids) {
    if (!Need(3))
      return 0;
    const u8 b0 = p_[0], b1 = p_[1], b2 = p_[2];
    p_ += 3;
    const u32 value = (static_cast<u32>(b0 & 0x3f) << 16) | (static_cast<u32>(b1) << 8) | b2;
    switch (b0 >> 6) {
      case 0:
        return (value == 0 || value > form_ids.size()) ? 0 : form_ids[value - 1];
      case 1:
        return value;
      case 2:
        return 0xff000000u | value;
      default:
        return 0;
    }
  }

 private:
  const u8* begin_;
  const u8* p_;
  const u8* end_;
  bool ok_ = true;
};

// A script's own members, before the parent chain is folded in.
struct ScriptDef {
  u32 parent = 0;  // string table index, 0 = none
  u32 first_member = 0;
  u32 member_count = 0;
  // Filled lazily: the flattened list, parent chain first. Measured: with the
  // parent's members ahead of the script's own, the declared type of every one
  // of 118,049 sampled variables matches the value actually stored; the other
  // order agrees only 76.5% of the time. The flattened length matches the value
  // count on all 125,195 instances, which is what makes naming them possible.
  u32 first_flat = 0;
  u32 flat_count = 0;
  bool flattened = false;
};

bool ReadValue(Reader& r, PapyrusValue* out) {
  const u8 tag = r.U8();
  out->type = static_cast<PapyrusValueType>(tag);
  switch (out->type) {
    case PapyrusValueType::kNone:
    case PapyrusValueType::kInt:
    case PapyrusValueType::kFloat:
    case PapyrusValueType::kBool:
      out->data = r.U32();
      return r.ok();
    case PapyrusValueType::kString:
      out->name = r.U32();
      return r.ok();
    case PapyrusValueType::kRef:
    case PapyrusValueType::kRefArray:
      out->name = r.U32();
      out->data = r.U64();
      return r.ok();
    case PapyrusValueType::kStringArray:
    case PapyrusValueType::kIntArray:
    case PapyrusValueType::kFloatArray:
    case PapyrusValueType::kBoolArray:
      out->data = r.U64();
      return r.ok();
    default:
      // An unknown tag means the walk has desynced. Stopping here is the point:
      // every later byte would be read at the wrong place.
      RX_WARN("save: papyrus value tag {} at heap offset {}, stopping the walk", tag,
              r.offset() - 1);
      return false;
  }
}

// The header of one data block: id, state, and how many values follow.
struct BlockHeader {
  u64 id = 0;
  u32 state = 0;
  u32 count = 0;
};

bool ReadBlockHeader(Reader& r, BlockHeader* out) {
  out->id = r.U64();
  const u8 flag = r.U8();
  out->state = r.U32();
  r.U32();  // always 0 across all 125,195 rows; nothing in the file explains it
  if (flag & kDataFlagExtraField)
    r.U32();
  out->count = r.U32();
  return r.ok() && out->count <= kMaxMembers;
}

}  // namespace

f32 PapyrusValue::AsFloat() const {
  const u32 bits = static_cast<u32>(data);
  f32 v;
  std::memcpy(&v, &bits, 4);
  return v;
}

bool ReadPapyrusHeap(ByteSpan table, const base::Vector<u32>& form_ids, PapyrusHeap* out) {
  PapyrusHeap heap;
  heap.table_bytes = table.size();
  Reader r(table);

  heap.version = r.U16();
  const u32 string_count = r.U32();
  // Every string is at least its own two byte length, so a count that cannot
  // fit in what is left is a corrupt table.
  if (!r.ok() || static_cast<u64>(string_count) * 2 > r.remaining())
    return false;
  heap.strings.resize(string_count);
  for (u32 i = 0; i < string_count && r.ok(); ++i)
    heap.strings[i] = r.WString();
  if (!r.ok())
    return false;

  // Script definitions. Only their member lists are wanted, and only while the
  // instances are being named, so they stay local to this function.
  const u32 script_count = r.U32();
  heap.script_count = script_count;
  if (!r.ok() || static_cast<u64>(script_count) * 12 > r.remaining())
    return false;
  base::UnorderedMap<base::String, u32> script_by_name;
  base::Vector<ScriptDef> defs;
  base::Vector<u32> members;  // (name, type) pairs flattened to just the names
  defs.reserve(script_count);
  for (u32 i = 0; i < script_count; ++i) {
    const u32 name = r.U32();
    ScriptDef def;
    def.parent = r.U32();
    def.member_count = r.U32();
    if (!r.ok() || def.member_count > kMaxMembers ||
        static_cast<u64>(def.member_count) * 8 > r.remaining())
      return false;
    def.first_member = static_cast<u32>(members.size());
    for (u32 m = 0; m < def.member_count; ++m) {
      members.push_back(r.U32());
      r.U32();  // the member's declared type, only used to check the layout
    }
    if (name < string_count)
      script_by_name[Lower(heap.strings[name])] = static_cast<u32>(defs.size());
    defs.push_back(def);
  }
  if (!r.ok())
    return false;

  // Instance header rows: a fixed 20 bytes, checked by the block walk below
  // finding each row's id again at the head of its own data.
  const u32 instance_count = r.U32();
  if (!r.ok() || static_cast<u64>(instance_count) * 20 > r.remaining())
    return false;
  heap.instances.resize(instance_count);
  for (u32 i = 0; i < instance_count; ++i) {
    PapyrusInstance& inst = heap.instances[i];
    inst.id = r.U64();
    inst.script = r.U32();
    inst.kind = static_cast<u8>(r.U16());
    inst.alias_id = r.U16();
    inst.form_id = r.RefId(form_ids);
    r.U8();  // 0 or 1 across the table, tracking nothing else in the row
  }
  if (!r.ok())
    return false;

  // Heap references: an id and a class name, no form. Walked past.
  heap.reference_count = r.U32();
  if (!r.ok() || static_cast<u64>(heap.reference_count) * 12 > r.remaining())
    return false;
  base::Vector<u64> reference_ids;
  reference_ids.reserve(heap.reference_count);
  for (u32 i = 0; i < heap.reference_count; ++i) {
    reference_ids.push_back(r.U64());
    r.U32();
  }

  const u32 array_count = r.U32();
  if (!r.ok() || static_cast<u64>(array_count) * 13 > r.remaining())
    return false;
  heap.arrays.resize(array_count);
  for (u32 i = 0; i < array_count; ++i) {
    PapyrusArray& array = heap.arrays[i];
    array.id = r.U64();
    array.element_type = r.U8();
    if (array.element_type == static_cast<u8>(PapyrusValueType::kRef))
      array.element_class = r.U32();
    array.value_count = r.U32();
    if (!r.ok() || array.value_count > kMaxMembers)
      return false;
  }

  r.U32();  // a runtime counter that means nothing outside Bethesda's own VM
  heap.active_script_count = r.U32();
  if (!r.ok() || static_cast<u64>(heap.active_script_count) * 5 > r.remaining())
    return false;
  for (u32 i = 0; i < heap.active_script_count; ++i) {
    r.U32();
    r.U8();
  }
  if (!r.ok())
    return false;

  // Flattens a script's member names, parent chain first. Memoized into `flat`
  // because the chains are shared and there are six figures of instances.
  base::Vector<u32> flat;
  // The parent chain is file-supplied and only bounded by the script count, so
  // the recursion needs its own limit: a save naming a hundred thousand scripts
  // in a chain would otherwise run the stack out. Real chains are a handful of
  // links deep (ObjectReference -> Form -> ...).
  constexpr u32 kMaxScriptChain = 256;
  auto flatten = [&](u32 script_index, u32 depth, auto&& self) -> void {
    ScriptDef& def = defs[script_index];
    if (def.flattened)
      return;
    def.flattened = true;  // also the guard against a cyclic parent chain
    const u32 parent = def.parent;
    const u32* parent_index = parent < string_count
                                  ? script_by_name.find(Lower(heap.strings[parent]))
                                  : nullptr;
    def.first_flat = static_cast<u32>(flat.size());
    if (parent_index != nullptr && *parent_index != script_index &&
        depth < kMaxScriptChain) {
      self(*parent_index, depth + 1, self);
      const ScriptDef& up = defs[*parent_index];
      // Re-read: flattening the parent may have grown `flat` and moved `def`.
      ScriptDef& mine = defs[script_index];
      mine.first_flat = static_cast<u32>(flat.size());
      // Copy out before pushing: push_back takes a reference, and a growth frees
      // the old block before it constructs from that reference, so feeding it an
      // element of the same vector is a use-after-free.
      for (u32 i = 0; i < up.flat_count; ++i) {
        const u32 inherited = flat[up.first_flat + i];
        flat.push_back(inherited);
      }
    }
    ScriptDef& mine = defs[script_index];
    for (u32 i = 0; i < mine.member_count; ++i)
      flat.push_back(members[mine.first_member + i]);
    mine.flat_count = static_cast<u32>(flat.size() - mine.first_flat);
  };

  // Instance data. Each block repeats its header row's id, in the same order,
  // which is the check that the 20 byte row above is right.
  heap.variables.reserve(instance_count * 6);
  for (u32 i = 0; i < instance_count; ++i) {
    PapyrusInstance& inst = heap.instances[i];
    BlockHeader block;
    if (!ReadBlockHeader(r, &block) || block.id != inst.id) {
      RX_WARN("save: papyrus instance {} data id {:#x} does not match its row {:#x}", i, block.id,
              inst.id);
      return false;
    }
    inst.state = block.state;
    inst.first_variable = static_cast<u32>(heap.variables.size());
    inst.variable_count = block.count;

    const u32* script_index = inst.script < string_count
                                  ? script_by_name.find(Lower(heap.strings[inst.script]))
                                  : nullptr;
    const u32* names = nullptr;
    if (script_index != nullptr) {
      flatten(*script_index, 0, flatten);
      const ScriptDef& def = defs[*script_index];
      if (def.flat_count == block.count)
        names = flat.data() + def.first_flat;
    }
    for (u32 v = 0; v < block.count; ++v) {
      PapyrusVariable var;
      var.name = names != nullptr ? names[v] : 0;
      if (!ReadValue(r, &var.value))
        return false;
      heap.variables.push_back(var);
    }
    if (names == nullptr)
      heap.unnamed_variables += block.count;
  }

  // Reference data: the same block shape, and empty on every row of the
  // reference save, which is why nothing here can say what form one stands for.
  for (u32 i = 0; i < heap.reference_count; ++i) {
    BlockHeader block;
    if (!ReadBlockHeader(r, &block) || block.id != reference_ids[i])
      return false;
    for (u32 v = 0; v < block.count; ++v) {
      PapyrusValue value;
      if (!ReadValue(r, &value))
        return false;
    }
  }

  // Array data: an id and then its elements, no block header.
  for (u32 i = 0; i < array_count; ++i) {
    PapyrusArray& array = heap.arrays[i];
    if (r.U64() != array.id)
      return false;
    array.first_value = static_cast<u32>(heap.array_values.size());
    for (u32 v = 0; v < array.value_count; ++v) {
      PapyrusValue value;
      if (!ReadValue(r, &value))
        return false;
      heap.array_values.push_back(value);
    }
  }
  if (!r.ok())
    return false;

  heap.consumed_bytes = r.offset();
  heap.present = true;
  *out = base::move(heap);
  return true;
}

u64 PapyrusRestore::KeyOf(GlobalFormId form, u32 alias_id) {
  // packed() uses the low 48 bits, so the alias index sits above them.
  return (static_cast<u64>(alias_id & 0xfffu) << 48) | form.packed();
}

const base::String& PapyrusRestore::Str(u32 index) const {
  static const base::String kEmpty;
  return index < heap_.strings.size() ? heap_.strings[index] : kEmpty;
}

base::Span<const PapyrusVariable> PapyrusRestore::VariablesOf(
    const PapyrusInstance& instance) const {
  if (instance.first_variable + instance.variable_count > heap_.variables.size())
    return base::Span<const PapyrusVariable>(nullptr, 0);
  return base::Span<const PapyrusVariable>(heap_.variables.data() + instance.first_variable,
                                           instance.variable_count);
}

base::Span<const PapyrusValue> PapyrusRestore::ValuesOf(const PapyrusArray& array) const {
  if (array.first_value + array.value_count > heap_.array_values.size())
    return base::Span<const PapyrusValue>(nullptr, 0);
  return base::Span<const PapyrusValue>(heap_.array_values.data() + array.first_value,
                                        array.value_count);
}

namespace {

// Finds the first entry with this key in a vector sorted by it.
const base::Pair<u64, u32>* FirstWithKey(const base::Vector<base::Pair<u64, u32>>& sorted, u64 key) {
  size_t lo = 0, hi = sorted.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (sorted[mid].first < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < sorted.size() && sorted[lo].first == key ? &sorted[lo] : nullptr;
}

}  // namespace

void PapyrusRestore::Build(PapyrusHeap heap, const FormRemap& remap, PapyrusRestoreStats* stats) {
  heap_ = base::move(heap);
  PapyrusRestoreStats tally;
  tally.instances = static_cast<u32>(heap_.instances.size());

  targets_.resize(heap_.instances.size());
  index_.reserve(heap_.instances.size());
  by_heap_id_.reserve(heap_.instances.size());

  for (u32 i = 0; i < heap_.instances.size(); ++i) {
    const PapyrusInstance& inst = heap_.instances[i];
    by_heap_id_.push_back({inst.id, i});
    if (inst.kind == 2) {
      ++tally.magic_effect;
      continue;
    }
    if (inst.kind != 0) {
      ++tally.other_kind;
      continue;
    }
    if (inst.form_id == 0) {
      ++tally.no_form;
      continue;
    }
    GlobalFormId form;
    FormRemap::Refusal why = FormRemap::Refusal::kNone;
    if (!remap.Map(inst.form_id, &form, &why)) {
      if (why != FormRemap::Refusal::kCreated) {
        ++tally.missing_plugin;
        continue;
      }
      // A reference the save spawned. No plugin has a record for it, so the
      // apply layer hands it a synthetic handle in the created-reference slot
      // and the streamer places it under that; keying its scripts the same way
      // is what lets them find each other.
      ++tally.created_form;
      form = GlobalFormId{kCreatedReferencePlugin, inst.form_id & 0xffffff};
    }
    PapyrusTarget& target = targets_[i];
    target.form = form;
    target.alias_id = inst.alias_id == kNoAlias ? kNoAlias : inst.alias_id;
    target.valid = true;
    index_.push_back({KeyOf(form, inst.alias_id), i});
    ++tally.indexed;
    tally.variables += inst.variable_count;
    if (inst.state != 0)
      ++tally.non_default_states;
  }

  arrays_.reserve(heap_.arrays.size());
  for (u32 i = 0; i < heap_.arrays.size(); ++i)
    arrays_.push_back({heap_.arrays[i].id, i});
  tally.arrays = static_cast<u32>(heap_.arrays.size());

  auto by_first = [](const base::Pair<u64, u32>& a, const base::Pair<u64, u32>& b) {
    return a.first < b.first;
  };
  base::Sort(index_.begin(), index_.end(), by_first);
  base::Sort(by_heap_id_.begin(), by_heap_id_.end(), by_first);
  base::Sort(arrays_.begin(), arrays_.end(), by_first);

  if (stats != nullptr)
    *stats = tally;
}

const PapyrusInstance* PapyrusRestore::Find(GlobalFormId form,
                                            u32 alias_id,
                                            base::StringRef script) const {
  const u64 key = KeyOf(form, alias_id);
  const base::Pair<u64, u32>* it = FirstWithKey(index_, key);
  if (it == nullptr)
    return nullptr;
  const base::Pair<u64, u32>* end = index_.data() + index_.size();
  for (; it != end && it->first == key; ++it) {
    const PapyrusInstance& inst = heap_.instances[it->second];
    if (IEquals(Str(inst.script), script))
      return &inst;
  }
  return nullptr;
}

const PapyrusArray* PapyrusRestore::FindArray(u64 heap_id) const {
  const base::Pair<u64, u32>* it = FirstWithKey(arrays_, heap_id);
  return it == nullptr ? nullptr : &heap_.arrays[it->second];
}

PapyrusTarget PapyrusRestore::Resolve(u64 heap_id) const {
  if (heap_id == 0)
    return {};
  const base::Pair<u64, u32>* it = FirstWithKey(by_heap_id_, heap_id);
  // Not an instance: it is one of the formless heap references, which nothing
  // in the file gives a form to.
  return it == nullptr ? PapyrusTarget{} : targets_[it->second];
}

}  // namespace rx::bethesda

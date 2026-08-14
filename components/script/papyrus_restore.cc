#include "components/script/papyrus_restore.h"

#include <base/memory/move.h>

#include "components/bethesda/savegame_apply.h"
#include "components/script/papyrus/alias_handle.h"
#include "components/script/papyrus/vm.h"
#include "core/log.h"

namespace rx::script {
namespace {

using papyrus::ArrayRef;
using papyrus::ObjectRef;
using papyrus::Value;

// The engine handle for a form the save named, or 0. A script on a quest alias
// is addressed by the alias handle the quest system already uses, which is why
// the save's alias index has to survive the parse.
u64 HandleOf(const bethesda::PapyrusTarget& target) {
  if (!target.valid)
    return 0;
  if (target.alias_id == 0xffff)
    return target.form.packed();
  return papyrus::EncodeAliasHandle(target.form.packed(), target.alias_id);
}

// The element type name ArrayCreate wants, spelled the way the record-baked
// property seeding spells it.
const char* ElementTypeName(u8 tag) {
  switch (static_cast<bethesda::PapyrusValueType>(tag)) {
    case bethesda::PapyrusValueType::kString:
      return "String";
    case bethesda::PapyrusValueType::kInt:
      return "Int";
    case bethesda::PapyrusValueType::kFloat:
      return "Float";
    case bethesda::PapyrusValueType::kBool:
      return "Bool";
    default:
      return "";
  }
}

}  // namespace

void PapyrusRestorer::Build(bethesda::PapyrusHeap heap, const bethesda::FormRemap& remap) {
  restore_.Build(base::move(heap), remap, &stats_);
}

Value PapyrusRestorer::ValueFor(papyrus::VirtualMachine& vm,
                                const bethesda::PapyrusValue& value) {
  switch (value.type) {
    case bethesda::PapyrusValueType::kNone:
      return Value();
    case bethesda::PapyrusValueType::kInt:
      return Value::Int(value.AsInt());
    case bethesda::PapyrusValueType::kFloat:
      return Value::Float(value.AsFloat());
    case bethesda::PapyrusValueType::kBool:
      return Value::Bool(value.AsBool());
    case bethesda::PapyrusValueType::kString:
      return Value::Str(restore_.Str(value.name));
    case bethesda::PapyrusValueType::kRef: {
      const u64 handle = HandleOf(restore_.Resolve(value.data));
      // The save points at something this run cannot name: a heap object with
      // no form of its own, or a form from a plugin that is not loaded. None is
      // the honest answer; inventing a handle would point the script at the
      // wrong object.
      if (handle == 0 && value.data != 0)
        ++refs_dropped_;
      return handle == 0 ? Value() : Value::Object(ObjectRef{handle});
    }
    default:
      break;
  }
  if (!value.IsArray() || value.data == 0)
    return Value();
  if (const u32* built = arrays_.find(value.data))
    return Value::Array(ArrayRef{*built});
  const bethesda::PapyrusArray* array = restore_.FindArray(value.data);
  if (array == nullptr)
    return Value();
  base::Span<const bethesda::PapyrusValue> values = restore_.ValuesOf(*array);
  const base::String element = array->element_type ==
                                       static_cast<u8>(bethesda::PapyrusValueType::kRef)
                                   ? restore_.Str(array->element_class)
                                   : base::String(ElementTypeName(array->element_type));
  ArrayRef ref = vm.ArrayCreate(element, static_cast<i32>(values.size()));
  // Recorded before filling it, so an array holding itself terminates.
  arrays_[value.data] = ref.id;
  ++arrays_built_;
  for (size_t i = 0; i < values.size(); ++i)
    vm.ArraySet(ref, static_cast<i32>(i), ValueFor(vm, values[i]));
  return Value::Array(ref);
}

bool PapyrusRestorer::Apply(papyrus::VirtualMachine& vm,
                            ObjectRef instance,
                            const base::String& script) {
  if (restore_.empty() || instance.handle == 0)
    return false;
  bethesda::GlobalFormId form;
  u32 alias = 0xffff;
  if (papyrus::IsAliasHandle(instance.handle)) {
    const u64 quest = papyrus::AliasHandleQuest(instance.handle);
    form.plugin = static_cast<u16>(quest >> 32);
    form.local_id = static_cast<u32>(quest);
    alias = papyrus::AliasHandleAliasId(instance.handle);
  } else {
    form.plugin = static_cast<u16>(instance.handle >> 32);
    form.local_id = static_cast<u32>(instance.handle);
  }

  const bethesda::PapyrusInstance* saved = restore_.Find(form, alias, script);
  if (saved == nullptr)
    return false;

  ++instances_;
  // Most of the heap goes back in as cells stream in, over minutes, so a single
  // tally at load would only ever show the quest scripts. One line per two
  // thousand instances is the cheapest way to see the streamed half land.
  if (instances_ % 2000 == 0)
    LogRestored();
  if (saved->state != 0) {
    // A script sitting in the wrong state runs the wrong event handlers, so
    // this matters as much as the variables do. Set, not entered: OnBeginState
    // already ran when play put it there.
    vm.GotoState(instance, restore_.Str(saved->state));
    ++states_;
  }
  for (const bethesda::PapyrusVariable& var : restore_.VariablesOf(*saved)) {
    const base::String& name = restore_.Str(var.name);
    if (name.empty())
      continue;
    if (vm.SetDeclaredMember(instance, name, ValueFor(vm, var.value)))
      ++members_;
    else
      ++members_undeclared_;
  }
  return true;
}

void PapyrusRestorer::LogCoverage() const {
  const bethesda::PapyrusHeap& heap = restore_.heap();
  RX_INFO(
      "papyrus: the save holds {} script instances with {} member variables; {} are addressable "
      "here ({} variables, {} of them on references the save spawned), {} are active magic "
      "effects, {} are of a kind the engine does not model",
      stats_.instances, static_cast<u32>(heap.variables.size()), stats_.indexed, stats_.variables,
      stats_.created_form, stats_.magic_effect, stats_.other_kind);
  if (stats_.missing_plugin != 0)
    RX_WARN("papyrus: {} instances name a plugin this run has not loaded", stats_.missing_plugin);
  // Held for the whole session, so it is worth saying out loud how much.
  const size_t bytes = heap.variables.size() * sizeof(bethesda::PapyrusVariable) +
                       heap.instances.size() * sizeof(bethesda::PapyrusInstance) +
                       heap.array_values.size() * sizeof(bethesda::PapyrusValue) +
                       heap.strings.size() * (sizeof(base::String) + 24) +
                       stats_.instances * 24;  // the two id indexes and the target table
  RX_INFO("papyrus: the heap index holds about {} MB for the session", bytes / (1024 * 1024));
}

void PapyrusRestorer::LogRestored() const {
  RX_INFO("papyrus: restored {} instances, {} member variables, {} put back into a named state",
          instances_, members_, states_);
  if (members_undeclared_ != 0)
    RX_WARN("papyrus: {} saved variables name a member the script here does not declare",
            members_undeclared_);
  if (refs_dropped_ != 0)
    RX_INFO("papyrus: {} object values named something this run cannot address, left as None",
            refs_dropped_);
  if (arrays_built_ != 0)
    RX_INFO("papyrus: {} arrays rebuilt in the VM heap", arrays_built_);
}

}  // namespace rx::script

#include "script/script_system.h"

#include "core/log.h"
#include "script/papyrus/alias_handle.h"
#include "script/papyrus/vm.h"

namespace rx::script {

using papyrus::ArrayRef;
using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;

ScriptSystem::ScriptSystem(bethesda::Game game, asset::Vfs* vfs, skyrim::SkyrimBindings* bindings)
    : vfs_(vfs), guest_(game) {
  // The "skyrim" native surface (GetForm, GetActorValue, ...) is record-backed
  // and game agnostic, so it serves every Bethesda game; register it for all of
  // them, not just Skyrim, so the Fallout microvms expose the same API to mods.
  if (bindings && (game == bethesda::Game::kSkyrimSe || game == bethesda::Game::kFallout4 ||
                   game == bethesda::Game::kFallout76 || game == bethesda::Game::kStarfield)) {
    skyrim::RegisterSkyrimNatives(guest_.natives(), bindings);
    // Let the VM resolve a bare reference's type (placed/alias/spawned refs with
    // no script) so `GetReference() as Actor` and similar casts work; without it
    // the Civil War reinforcement classifiers fail on every cell-less soldier.
    guest_.set_type_resolver([bindings](ObjectRef ref, const std::string& type) {
      return bindings->RefIsType(ref, type);
    });
  }
  guest_.Start();
}

ScriptSystem::~ScriptSystem() { guest_.Stop(); }

std::string ScriptSystem::EnsureScriptLoaded(const std::string& name) {
  if (name.empty()) return "";
  // Already loaded?
  bool present = guest_.SubmitFor([name](VirtualMachine& vm) { return vm.HasScript(name); }).get();
  if (present) return name;

  auto blob = vfs_->Read("scripts/" + name + ".pex");
  if (!blob) {
    RX_DEBUG("script: scripts/{}.pex not found", name);
    return "";
  }
  std::vector<u8> bytes(blob->begin(), blob->end());
  std::string type =
      guest_
          .SubmitFor([b = std::move(bytes)](VirtualMachine& vm) {
            return vm.LoadScript(ByteSpan(b.data(), b.size()));
          })
          .get();
  if (type.empty()) return "";

  // Load the parent chain so inherited natives and members resolve.
  std::string parent =
      guest_.SubmitFor([type](VirtualMachine& vm) { return vm.ParentClassOf(type); }).get();
  if (!parent.empty()) EnsureScriptLoaded(parent);
  return type;
}

namespace {

// Writes a baked VMAD property onto a live instance. Arrays are built in the VM
// heap, which is why this runs on the guest thread with the VM in hand. Object
// values are keyed by form id, the engine's object identity.
void SeedProperty(VirtualMachine& vm, ObjectRef inst, const bethesda::ScriptProperty& p) {
  switch (p.type) {
    case 1: {
      // A quest alias property (alias_id set) becomes an alias handle the VM can
      // call ReferenceAlias methods on; a plain object property is its form id.
      const u64 handle = p.object_value.alias_id != 0xffff
                             ? papyrus::EncodeAliasHandle(inst.handle, p.object_value.alias_id)
                             : p.object_value.form_id;
      vm.SetProperty(inst, p.name, Value::Object(ObjectRef{handle}));
      break;
    }
    case 2:
      vm.SetProperty(inst, p.name, Value::Str(p.string_value));
      break;
    case 3:
      vm.SetProperty(inst, p.name, Value::Int(p.int_value));
      break;
    case 4:
      vm.SetProperty(inst, p.name, Value::Float(p.float_value));
      break;
    case 5:
      vm.SetProperty(inst, p.name, Value::Bool(p.bool_value));
      break;
    case 11: {
      ArrayRef a = vm.ArrayCreate("", static_cast<i32>(p.object_array.size()));
      for (size_t i = 0; i < p.object_array.size(); ++i)
        vm.ArraySet(a, static_cast<i32>(i), Value::Object(ObjectRef{p.object_array[i].form_id}));
      vm.SetProperty(inst, p.name, Value::Array(a));
      break;
    }
    case 12: {
      ArrayRef a = vm.ArrayCreate("String", static_cast<i32>(p.string_array.size()));
      for (size_t i = 0; i < p.string_array.size(); ++i)
        vm.ArraySet(a, static_cast<i32>(i), Value::Str(p.string_array[i]));
      vm.SetProperty(inst, p.name, Value::Array(a));
      break;
    }
    case 13: {
      ArrayRef a = vm.ArrayCreate("Int", static_cast<i32>(p.int_array.size()));
      for (size_t i = 0; i < p.int_array.size(); ++i)
        vm.ArraySet(a, static_cast<i32>(i), Value::Int(p.int_array[i]));
      vm.SetProperty(inst, p.name, Value::Array(a));
      break;
    }
    case 14: {
      ArrayRef a = vm.ArrayCreate("Float", static_cast<i32>(p.float_array.size()));
      for (size_t i = 0; i < p.float_array.size(); ++i)
        vm.ArraySet(a, static_cast<i32>(i), Value::Float(p.float_array[i]));
      vm.SetProperty(inst, p.name, Value::Array(a));
      break;
    }
    case 15: {
      ArrayRef a = vm.ArrayCreate("Bool", static_cast<i32>(p.bool_array.size()));
      for (size_t i = 0; i < p.bool_array.size(); ++i)
        vm.ArraySet(a, static_cast<i32>(i), Value::Bool(p.bool_array[i] != 0));
      vm.SetProperty(inst, p.name, Value::Array(a));
      break;
    }
    default:
      break;
  }
}

}  // namespace

std::vector<ObjectRef> ScriptSystem::AttachScripts(u64 form_id,
                                                   const bethesda::ScriptAttachment& att) {
  std::vector<ObjectRef> instances;
  for (const bethesda::ScriptEntry& entry : att.scripts) {
    std::string type = EnsureScriptLoaded(entry.name);
    if (type.empty()) {
      if (warned_unloadable_.insert(entry.name).second)
        RX_WARN("script: cannot attach {}, .pex missing or not executable", entry.name);
      continue;
    }
    ObjectRef inst =
        guest_
            .SubmitFor([type, form_id](VirtualMachine& vm) {
              return vm.CreateInstanceWithHandle(type, form_id);
            })
            .get();
    if (inst.handle == 0) continue;  // already instantiated on this form

    guest_.Submit([inst, props = entry.properties](VirtualMachine& vm) {
      for (const bethesda::ScriptProperty& p : props) SeedProperty(vm, inst, p);
    });
    guest_.RaiseEvent(inst, "OnInit");
    instances.push_back(inst);
  }
  // Signal the form went live so the managed world can react (FormLoaded).
  if (on_attach_ && !instances.empty()) on_attach_(form_id);
  return instances;
}

void ScriptSystem::Tick(f32 dt) { guest_.Tick(dt); }

size_t ScriptSystem::loaded_script_count() {
  return guest_.SubmitFor([](VirtualMachine& vm) { return vm.script_count(); }).get();
}

}  // namespace rx::script

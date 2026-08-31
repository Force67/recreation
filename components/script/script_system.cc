#include "components/script/script_system.h"
#include "core/log.h"

#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include "components/script/papyrus/alias_handle.h"
#include "components/script/papyrus/vm.h"

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
    guest_.set_type_resolver([bindings](ObjectRef ref, const base::String& type) {
      return bindings->RefIsType(ref, type);
    });
  }
  guest_.Start();
}

ScriptSystem::~ScriptSystem() {
  guest_.Stop();
}

base::String ScriptSystem::EnsureScriptLoaded(const base::String& name) {
  if (name.empty())
    return "";
  // Already loaded?
  bool present = guest_.SubmitFor([name](VirtualMachine& vm) { return vm.HasScript(name); }).get();
  if (present)
    return name;

  auto blob = vfs_->Read("scripts/" + name + ".pex");
  if (!blob) {
    RX_DEBUG("script: scripts/{}.pex not found", name);
    return "";
  }
  base::Vector<u8> bytes(blob->begin(), blob->end());
  base::String type = guest_
                          .SubmitFor([b = base::move(bytes)](VirtualMachine& vm) {
                            return vm.LoadScript(ByteSpan(b.data(), b.size()));
                          })
                          .get();
  if (type.empty())
    return "";

  // Load the parent chain so inherited natives and members resolve.
  base::String parent =
      guest_.SubmitFor([type](VirtualMachine& vm) { return vm.ParentClassOf(type); }).get();
  if (!parent.empty())
    EnsureScriptLoaded(parent);
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

// What one visit to the guest thread settles about a script attachment.
struct Attached {
  ObjectRef instance;
  bool attached = false;
  bool restored = false;
};

}  // namespace

base::Vector<ObjectRef> ScriptSystem::AttachScripts(u64 form_id,
                                                    const bethesda::ScriptAttachment& att) {
  return AttachScriptsWithStatus(form_id, att).created;
}

ScriptSystem::AttachmentResult ScriptSystem::AttachScriptsWithStatus(
    u64 form_id,
    const bethesda::ScriptAttachment& att) {
  AttachmentResult result;
  for (const bethesda::ScriptEntry& entry : att.scripts) {
    base::String type = EnsureScriptLoaded(entry.name);
    if (type.empty()) {
      result.complete = false;
      if (warned_unloadable_.insert(entry.name))
        RX_WARN("script: cannot attach {}, .pex missing or not executable", entry.name);
      continue;
    }
    // One hop to the guest thread for the whole attachment. Seeding the baked
    // properties used to be a second, asynchronous one; folding it in keeps the
    // savegame restore, which has to run after the properties and before OnInit,
    // on the same visit.
    Attached done =
        guest_
            .SubmitFor([type, form_id, props = entry.properties, this](VirtualMachine& vm) {
              Attached out;
              out.instance = vm.CreateInstanceWithHandle(type, form_id);
              out.attached = out.instance.handle != 0 ||
                             vm.HasAttachedScript(ObjectRef{form_id}, type);
              if (out.instance.handle == 0)
                return out;
              for (const bethesda::ScriptProperty& p : props)
                SeedProperty(vm, out.instance, p);
              // The save has the last word over the record's baked values: the
              // record says what the form starts as, the save says what play
              // made of it.
              out.restored = on_restore_ && on_restore_(vm, out.instance, type);
              return out;
            })
            .get();
    if (!done.attached) {
      result.complete = false;
      continue;
    }
    result.any_attached = true;
    if (done.instance.handle == 0)
      continue;  // already instantiated on this form

    // An instance the save owned was initialized once already, so OnInit does
    // not run again; that is what the game does on load too, and re-running it
    // would undo the state just restored.
    if (!done.restored)
      guest_.RaiseScriptEvent(done.instance, type, "OnInit");
    result.created.push_back(done.instance);
  }
  // Signal the form went live so the managed world can react (FormLoaded).
  if (on_attach_ && !result.created.empty())
    on_attach_(form_id);
  return result;
}

void ScriptSystem::RaiseFormLoadEvent(u64 form_id) {
  guest_.RaiseEventAll(ObjectRef{form_id}, "OnLoad", {});
}

void ScriptSystem::RaiseFormUnloadEvent(u64 form_id) {
  guest_.RaiseEventAll(ObjectRef{form_id}, "OnUnload", {});
}

void ScriptSystem::NotifyFormReloaded(u64 form_id) {
  if (!guest_.SubmitFor([form_id](VirtualMachine& vm) { return vm.IsAlive(ObjectRef{form_id}); })
           .get())
    return;
  RaiseFormLoadEvent(form_id);
  if (on_attach_)
    on_attach_(form_id);
}

void ScriptSystem::Tick(f32 dt) {
  guest_.Tick(dt);
}

size_t ScriptSystem::loaded_script_count() {
  return guest_.SubmitFor([](VirtualMachine& vm) { return vm.script_count(); }).get();
}

}  // namespace rx::script

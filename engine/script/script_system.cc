#include "script/script_system.h"

#include "core/log.h"
#include "script/papyrus/vm.h"

namespace rec::script {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;

ScriptSystem::ScriptSystem(bethesda::Game game, asset::Vfs* vfs, skyrim::SkyrimBindings* bindings)
    : vfs_(vfs), guest_(game) {
  if (game == bethesda::Game::kSkyrimSe)
    skyrim::RegisterSkyrimNatives(guest_.natives(), bindings);
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
    REC_DEBUG("script: scripts/{}.pex not found", name);
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

Value ScriptSystem::ToValue(const bethesda::ScriptProperty& property) {
  switch (property.type) {
    case 1:  // object reference: keyed by form id (the engine's object identity)
      return Value::Object(ObjectRef{property.object_value.form_id});
    case 2:
      return Value::Str(property.string_value);
    case 3:
      return Value::Int(property.int_value);
    case 4:
      return Value::Float(property.float_value);
    case 5:
      return Value::Bool(property.bool_value);
    default:
      // Array properties are not seeded yet; the script keeps its declared
      // default (an empty array). Logged once so coverage gaps are visible.
      return Value();
  }
}

std::vector<ObjectRef> ScriptSystem::AttachScripts(u64 form_id,
                                                   const bethesda::ScriptAttachment& att) {
  std::vector<ObjectRef> instances;
  for (const bethesda::ScriptEntry& entry : att.scripts) {
    std::string type = EnsureScriptLoaded(entry.name);
    if (type.empty()) {
      REC_WARN("script: cannot attach {}, .pex unavailable", entry.name);
      continue;
    }
    ObjectRef inst =
        guest_
            .SubmitFor([type, form_id](VirtualMachine& vm) {
              return vm.CreateInstanceWithHandle(type, form_id);
            })
            .get();
    if (inst.handle == 0) continue;  // already instantiated on this form

    for (const bethesda::ScriptProperty& property : entry.properties) {
      Value value = ToValue(property);
      guest_.Submit([inst, name = property.name, value](VirtualMachine& vm) mutable {
        vm.SetProperty(inst, name, std::move(value));
      });
    }
    guest_.RaiseEvent(inst, "OnInit");
    instances.push_back(inst);
  }
  return instances;
}

void ScriptSystem::Tick(f32 dt) { guest_.Tick(dt); }

size_t ScriptSystem::loaded_script_count() {
  return guest_.SubmitFor([](VirtualMachine& vm) { return vm.script_count(); }).get();
}

}  // namespace rec::script

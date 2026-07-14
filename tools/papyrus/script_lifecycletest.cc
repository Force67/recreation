// script_lifecycletest: deterministic reference attachment lifecycle checks.

#include <cstdio>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>

#include "asset/vfs.h"
#include "bethesda/script_attachment.h"
#include "script/papyrus/pex.h"
#include "script/papyrus/vm.h"
#include "script/script_system.h"

namespace {

using namespace rx;
using namespace rx::script::papyrus;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

struct PexBuilder {
  PexFile pex;
  std::unordered_map<std::string, StringIndex> strings;

  StringIndex String(const std::string& value) {
    auto it = strings.find(value);
    if (it != strings.end()) return it->second;
    const auto index = static_cast<StringIndex>(pex.string_table.size());
    pex.string_table.push_back(value);
    strings.emplace(value, index);
    return index;
  }

  VariableData Identifier(const std::string& value) {
    return {VariableData::Type::kIdentifier, String(value), 0, 0.0f, false};
  }
};

VariableData Integer(rx::i32 value) {
  return {VariableData::Type::kInteger, kInvalidString, value, 0.0f, false};
}

PexFile MakeScript(const std::string& name, bool lifecycle_events) {
  PexBuilder builder;
  Object object;
  object.name = builder.String(name);
  object.parent_class = builder.String("");

  State state;
  state.name = builder.String("");
  if (lifecycle_events) {
    const std::string events = name + "Events";
    object.variables.push_back({builder.String(events), builder.String("int"), 0, Integer(0)});

    Function on_init;
    on_init.return_type = builder.String("");
    on_init.code.push_back({Op::kAssign, {builder.Identifier(events), Integer(1)}, {}});
    state.functions.push_back({builder.String("OnInit"), std::move(on_init)});

    Function on_load;
    on_load.return_type = builder.String("");
    on_load.code.push_back({Op::kIAdd,
                            {builder.Identifier(events), builder.Identifier(events), Integer(10)},
                            {}});
    state.functions.push_back({builder.String("OnLoad"), std::move(on_load)});
  }
  object.states.push_back(std::move(state));
  builder.pex.objects.push_back(std::move(object));
  return std::move(builder.pex);
}

void AddScript(rx::script::ScriptSystem& system, const std::string& name,
               bool lifecycle_events = false) {
  std::string loaded =
      system.guest()
          .SubmitFor([pex = MakeScript(name, lifecycle_events)](VirtualMachine& vm) mutable {
            return vm.AddScript(std::move(pex));
          })
          .get();
  Check(("loaded " + name).c_str(), loaded == name);
}

bethesda::ScriptAttachment Attachment(std::initializer_list<const char*> names) {
  bethesda::ScriptAttachment attachment;
  for (const char* name : names) attachment.scripts.push_back({name, 0, {}});
  return attachment;
}

rx::i32 Events(rx::script::ScriptSystem& system, rx::u64 handle, const std::string& script) {
  return system.guest()
      .SubmitFor([handle, member = script + "Events"](VirtualMachine& vm) {
        Value* value = vm.MemberVar(ObjectRef{handle}, member);
        return value ? value->ToInt() : -1;
      })
      .get();
}

}  // namespace

int main() {
  std::puts("script_lifecycletest");
  asset::Vfs vfs;
  rx::script::ScriptSystem system(bethesda::Game::kSkyrimSe, &vfs, nullptr);

  AddScript(system, "LifecycleScript", true);
  const rx::u64 lifecycle_form = 0x1001;
  int loaded_notifications = 0;
  system.set_on_scripts_attached([&](rx::u64) { ++loaded_notifications; });
  const auto initial =
      system.AttachScriptsWithStatus(lifecycle_form, Attachment({"LifecycleScript"}));
  system.RaiseFormLoadEvent(lifecycle_form);
  Check("initial attachment is complete", initial.complete && initial.any_attached);
  Check("initial attachment created one script", initial.created.size() == 1);
  Check("initial OnInit runs before OnLoad",
        Events(system, lifecycle_form, "LifecycleScript") == 11);
  Check("initial managed load notification fires once", loaded_notifications == 1);

  const auto existing =
      system.AttachScriptsWithStatus(lifecycle_form, Attachment({"LifecycleScript"}));
  system.NotifyFormReloaded(lifecycle_form);
  Check("reload creates no replacement instance", existing.created.empty());
  Check("reload emits OnLoad without OnInit",
        Events(system, lifecycle_form, "LifecycleScript") == 21);
  Check("reload managed load notification fires once", loaded_notifications == 2);

  AddScript(system, "AvailableScript");
  const rx::u64 partial_form = 0x1002;
  const bethesda::ScriptAttachment partial = Attachment({"AvailableScript", "LateScript"});
  const auto first_partial = system.AttachScriptsWithStatus(partial_form, partial);
  Check("partial attachment reports incomplete",
        first_partial.any_attached && !first_partial.complete);
  Check("available script attaches once", first_partial.created.size() == 1);

  const auto still_partial = system.AttachScriptsWithStatus(partial_form, partial);
  Check("missing script remains retryable",
        still_partial.any_attached && !still_partial.complete && still_partial.created.empty());

  AddScript(system, "LateScript");
  const auto completed = system.AttachScriptsWithStatus(partial_form, partial);
  Check("later retry completes the attachment", completed.any_attached && completed.complete);
  Check("retry creates only the formerly missing script", completed.created.size() == 1);
  const auto idempotent = system.AttachScriptsWithStatus(partial_form, partial);
  Check("completed attachment is idempotent", idempotent.complete && idempotent.created.empty());

  AddScript(system, "PlacedScript", true);
  AddScript(system, "BaseScript", true);
  const rx::u64 combined_form = 0x1003;
  const auto combined =
      system.AttachScriptsWithStatus(combined_form, Attachment({"PlacedScript", "BaseScript"}));
  system.RaiseFormLoadEvent(combined_form);
  Check("placed and base scripts both attach", combined.complete && combined.created.size() == 2);
  Check("placed script receives one OnInit and one OnLoad",
        Events(system, combined_form, "PlacedScript") == 11);
  Check("base script receives one OnInit and one OnLoad",
        Events(system, combined_form, "BaseScript") == 11);
  system.NotifyFormReloaded(combined_form);
  Check("reload reaches each attached script exactly once",
        Events(system, combined_form, "PlacedScript") == 21 &&
            Events(system, combined_form, "BaseScript") == 21);

  if (g_failures != 0) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::puts("all checks passed");
  return 0;
}

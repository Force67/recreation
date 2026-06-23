#include "engine.h"

#include <cstdlib>
#include <filesystem>

#include "core/log.h"
#include "script/papyrus/value.h"

// Boots the managed (C#) scripting world over the Papyrus guest(s): registers
// the primary game plus every secondary content domain as managed domains, then
// routes engine gameplay events (death, item added, form load/unload, location
// change) into the managed event bus. Optional and gracefully absent.
namespace rec {

void BootManagedScripting(Engine& engine) {
  Engine* const self = &engine;
  // A replica client mirrors the server; its scripts must not mutate
  // authoritative state, so the managed world stays off here.
  if (self->script_bindings_ && self->script_bindings_->replica_mode()) {
    REC_INFO("managed: skipped on a replica client (server-authoritative)");
    return;
  }
  const char* dir = std::getenv("RECREATION_SCRIPTING_DIR");
  if (!dir || !*dir) {
    REC_INFO("managed: RECREATION_SCRIPTING_DIR unset, C# scripting disabled");
    return;
  }
  namespace fs = std::filesystem;
  const fs::path base(dir);
  const std::string assembly = (base / "Recreation.Scripting.dll").string();
  const std::string runtime_config = (base / "Recreation.Scripting.runtimeconfig.json").string();
  std::error_code ec;
  if (!fs::exists(assembly, ec) || !fs::exists(runtime_config, ec)) {
    REC_WARN("managed: Recreation.Scripting not found under {}", dir);
    return;
  }
  self->managed_ = std::make_unique<rec::script::host::ManagedHost>();
  // Register the primary (rendered) game first, then every secondary domain, so a
  // C# mod can reach Skyrim and Fallout content at the same time. Each domain
  // dispatches into its own guest, keeping the games' state isolated.
  self->managed_->AddDomain(
      bethesda::GameProfile::For(self->game_).name, self->scripts_->guest(),
      [self](const std::string& name) { return !self->scripts_->EnsureScriptLoaded(name).empty(); });
  for (auto& domain : self->extra_domains_) {
    ContentDomain* d = domain.get();
    self->managed_->AddDomain(d->profile().name, d->scripts()->guest(), [d](const std::string& name) {
      return !d->scripts()->EnsureScriptLoaded(name).empty();
    });
  }
  if (!self->managed_->Boot(/*dotnet_root=*/"", runtime_config, assembly)) {
    self->managed_.reset();  // unavailable: run without it
    return;
  }
  // Route gameplay events (death, item added, quest stage) from the bindings to
  // the managed event bus. The sink runs on the guest thread, so set it there to
  // avoid racing the guest, and have it only enqueue: the main loop drains the
  // queue into managed code (DrainEvents).
  auto* host = self->managed_.get();
  auto* binds = self->script_bindings_.get();
  self->scripts_->guest().Submit([binds, host](rec::script::papyrus::VirtualMachine&) {
    binds->set_event_sink(
        [host](const rec::script::host::ManagedEvent& e) { host->QueueEvent(e); });
  });
  // Notify the managed world when a form's scripts attach (it goes live), so
  // mods can attach C# behaviours to it. Fires on the main thread; QueueEvent is
  // drained next frame.
  self->scripts_->set_on_scripts_attached([host](u64 form) {
    host->QueueEvent({rec::script::host::ManagedEventId::kFormLoaded, form, 0, 0, 0.0f});
  });
  // The symmetric unload: when a tracked reference streams out, tell managed code
  // so per-form behaviours detach instead of ticking on a stale handle. Fires on
  // the main thread (cell streaming); QueueEvent is drained next frame.
  self->quest_world_.set_on_unregister([host](u64 form) {
    host->QueueEvent({rec::script::host::ManagedEventId::kFormUnloaded, form, 0, 0, 0.0f});
  });
  self->ctx_.managed = host;  // let subsystems (interaction, ...) raise managed events
}

}  // namespace rec

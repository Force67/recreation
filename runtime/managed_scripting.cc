#include "engine.h"

#include <cstdlib>
#include <filesystem>

#include <base/option.h>

#include "core/log.h"
#include "script/papyrus/value.h"
#if defined(RECREATION_HAS_UGUI)
#include "ugui_csharp_host.h"  // the C# ultragui scripting backend seam
#endif

// Boots the managed (C#) scripting world over the Papyrus guest(s): registers
// the primary game plus every secondary content domain as managed domains, then
// routes engine gameplay events (death, item added, form load/unload, location
// change) into the managed event bus. Optional and gracefully absent.
namespace rx {

// RECREATION_SCRIPTING_DIR points at the built managed assemblies; was read
// straight from the environment.
static base::Option<const char*> ScriptingDir{"scripting.dir", nullptr,
                                              "RECREATION_SCRIPTING_DIR"};

#if defined(RECREATION_HAS_UGUI)
// Bridges ultragui's handler dispatch to the managed world. Installed as the C#
// backend's dispatch callback; ctx is the ManagedHost.
static std::int32_t DispatchUiToManaged(void* ctx, const char* func_name, std::uint64_t widget) {
  return static_cast<rx::script::host::ManagedHost*>(ctx)->DispatchUi(func_name, widget);
}
#endif

void BootManagedScripting(Engine& engine) {
  Engine* const self = &engine;
  // The managed world runs in every role now, but filters which mods start by
  // realm: a host (server) runs server + shared mods, a replica client runs
  // client + shared, single-player runs everything. Authoritative mutations stay
  // gated by the bindings' replica_mode, so a client's scripts cannot diverge
  // from the server: they read state, request through RPC and react to events.
  const bool replica =
      self->script_bindings_ && self->script_bindings_->replica_mode();
  const std::int32_t realm = self->config_.host_server ? 0 : (replica ? 1 : 2);

  const char* dir = ScriptingDir.get();
  if (!dir || !*dir) {
    RX_INFO("managed: RECREATION_SCRIPTING_DIR unset, C# scripting disabled");
    return;
  }
  namespace fs = std::filesystem;
  const fs::path base(dir);
  const std::string assembly = (base / "Recreation.Scripting.dll").string();
  const std::string runtime_config = (base / "Recreation.Scripting.runtimeconfig.json").string();
  std::error_code ec;
  if (!fs::exists(assembly, ec) || !fs::exists(runtime_config, ec)) {
    RX_WARN("managed: Recreation.Scripting not found under {}", dir);
    return;
  }
  self->managed_ = std::make_unique<rx::script::host::ManagedHost>();
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
#if defined(RECREATION_HAS_UGUI)
  // Let managed UI handlers read and mutate live ugui widgets. Must precede Boot:
  // the table is handed to the managed entrypoint through the handshake.
  self->managed_->SetUiWidgetOps(rx::ugui_cs::GetWidgetOps());
#endif
#if RECREATION_HAS_NET
  // Hand the managed world the multiplayer RPC surface so mod scripts emit and
  // receive session calls. Subscriptions made now are forwarded once the session
  // opens (StartNetworking runs after this).
  self->managed_->SetRpcBridge(MakeManagedRpcBridge(*self));
#endif
  self->managed_->SetRealm(realm);  // server / client / standalone mod filtering
  if (!self->managed_->Boot(/*dotnet_root=*/"", runtime_config, assembly)) {
    self->managed_.reset();  // unavailable: run without it
    return;
  }
#if defined(RECREATION_HAS_UGUI)
  // Route every ultragui handler (button clicks, on_change) into the managed
  // world now that it is up. Inert until this point, so early UI input is safe.
  rx::ugui_cs::InstallHostDispatch(&DispatchUiToManaged, self->managed_.get());
#endif
  // Route gameplay events (death, item added, quest stage) from the bindings to
  // the managed event bus. The sink runs on the guest thread, so set it there to
  // avoid racing the guest, and have it only enqueue: the main loop drains the
  // queue into managed code (DrainEvents).
  auto* host = self->managed_.get();
  auto* binds = self->script_bindings_.get();
  self->scripts_->guest().Submit([binds, host](rx::script::papyrus::VirtualMachine&) {
    binds->set_event_sink(
        [host](const rx::script::host::ManagedEvent& e) { host->QueueEvent(e); });
  });
  // Notify the managed world when a form's scripts attach (it goes live), so
  // mods can attach C# behaviours to it. Fires on the main thread; QueueEvent is
  // drained next frame.
  self->scripts_->set_on_scripts_attached([host](u64 form) {
    host->QueueEvent({rx::script::host::ManagedEventId::kFormLoaded, form, 0, 0, 0.0f});
  });
  // The symmetric unload: when a tracked reference streams out, tell managed code
  // so per-form behaviours detach instead of ticking on a stale handle. Fires on
  // the main thread (cell streaming); QueueEvent is drained next frame.
  self->quest_world_.set_on_unregister([host](u64 form) {
    host->QueueEvent({rx::script::host::ManagedEventId::kFormUnloaded, form, 0, 0, 0.0f});
  });
  self->ctx_.managed = host;  // let subsystems (interaction, ...) raise managed events
}

}  // namespace rx

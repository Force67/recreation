#include "script/host/managed_host.h"

#include <utility>

#include "core/log.h"
#include "script/host/managed_gc_profile.h"
#include "script/papyrus_guest.h"

namespace rx::script::host {

ManagedHost::~ManagedHost() { Shutdown(); }

void ManagedHost::AddDomain(std::string name, PapyrusGuest& guest,
                            std::function<bool(const std::string&)> loader) {
  auto domain = std::make_unique<Domain>();
  domain->name = std::move(name);
  domain->ctx.guest = &guest;
  domain->ctx.loader = std::move(loader);
  domain->bridge = MakeScriptBridge(domain->ctx);  // ctx address is stable (heap)
  domains_.push_back(std::move(domain));
}

bool ManagedHost::Boot(const std::string& dotnet_root, const std::string& runtime_config,
                       const std::string& assembly) {
  if (domains_.empty()) {
    RX_WARN("managed: no content domain registered, scripting disabled");
    return false;
  }
  domain_table_.clear();
  domain_table_.reserve(domains_.size());
  for (const auto& domain : domains_)
    domain_table_.push_back({domain->name.c_str(), &domain->bridge});
  primary_guest_ = domains_[0]->ctx.guest;    // the thread the managed world runs on
  handshake_.bridge = &domains_[0]->bridge;  // primary, back-compatible
  handshake_.callbacks = {};
  handshake_.domain_count = static_cast<std::int32_t>(domain_table_.size());
  handshake_.domains = domain_table_.data();
  handshake_.ui_widget_ops = ui_widget_ops_;  // null when there is no UI backend
  handshake_.rpc = rpc_bridge_;  // emit/on null when networking is off
  handshake_.realm = realm_;     // server / client / standalone

  // Apply the per-platform / per-role GC and heap profile to the runtime before
  // it starts. Keyed by realm (dedicated server vs client vs standalone) and the
  // build platform; RECREATION_MANAGED_GC* env vars override.
  const std::string gc_profile = ResolveGcProfileName(realm_);
  RX_INFO("managed: GC profile '{}'", gc_profile);
  if (!clr_.Initialize(dotnet_root, runtime_config, assembly,
                       "Recreation.ScriptHost, Recreation.Scripting", "Main",
                       ManagedGcProfile(gc_profile))) {
    RX_INFO("managed: .NET host unavailable, scripting disabled");
    return false;
  }

  // The managed entrypoint binds the bridge, boots its mods, and fills in the
  // outbound callbacks we drive from here on.
  if (clr_.Invoke(&handshake_) != 0) {
    RX_WARN("managed: entrypoint returned nonzero, scripting disabled");
    return false;
  }
  available_ = true;
  RX_INFO("managed: scripting world online");
  return true;
}

void ManagedHost::RunManaged(const std::function<void()>& fn) {
  if (primary_guest_ && primary_guest_->running())
    // Dispatch runs fn inline if we are already on the guest thread (so a managed
    // callback that reaches back here cannot deadlock), else posts and blocks.
    primary_guest_->Dispatch([&fn](rx::script::papyrus::VirtualMachine&) {
      fn();
      return 0;
    });
  else
    fn();
}

void ManagedHost::Tick(float dt) {
  if (!available_ || !handshake_.callbacks.tick) return;
  auto tick = handshake_.callbacks.tick;
  RunManaged([tick, dt] { tick(dt); });
}

std::int32_t ManagedHost::DispatchUi(const char* func_name, std::uint64_t widget) {
  if (!available_ || !handshake_.callbacks.dispatch_ui) return 0;
  auto dispatch = handshake_.callbacks.dispatch_ui;
  std::int32_t result = 0;
  RunManaged([&] { result = dispatch(func_name, widget); });
  return result;
}

void ManagedHost::DispatchRpc(const char* name, std::int32_t sender,
                              std::int32_t from_server, const ApiValue* args,
                              std::int32_t argc) {
  if (!available_ || !handshake_.callbacks.dispatch_rpc) return;
  auto dispatch = handshake_.callbacks.dispatch_rpc;
  RunManaged([&] { dispatch(name, sender, from_server, args, argc); });
}

void ManagedHost::PublishEvent(const ManagedEvent& event) {
  if (!available_ || !handshake_.callbacks.publish_event) return;
  auto publish = handshake_.callbacks.publish_event;
  RunManaged([&] { publish(&event); });
}

void ManagedHost::QueueEvent(const ManagedEvent& event) {
  std::lock_guard<std::mutex> lock(event_mutex_);
  pending_events_.push_back(event);
}

void ManagedHost::DrainEvents() {
  if (!available_ || !handshake_.callbacks.publish_event) return;
  std::vector<ManagedEvent> events;
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    events.swap(pending_events_);
  }
  if (events.empty()) return;
  // Publish the whole batch under a single hop onto the guest thread, so N queued
  // events cost one round-trip, not N.
  auto publish = handshake_.callbacks.publish_event;
  RunManaged([&] {
    for (const ManagedEvent& e : events) publish(&e);
  });
}

void ManagedHost::Shutdown() {
  if (available_ && handshake_.callbacks.shutdown) {
    auto shutdown = handshake_.callbacks.shutdown;
    RunManaged([shutdown] { shutdown(); });
  }
  available_ = false;
  handshake_.callbacks = {};
  primary_guest_ = nullptr;
  clr_.Shutdown();
}

}  // namespace rx::script::host

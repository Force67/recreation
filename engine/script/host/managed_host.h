#ifndef RECREATION_SCRIPT_HOST_MANAGED_HOST_H_
#define RECREATION_SCRIPT_HOST_MANAGED_HOST_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "script/host/bridge.h"
#include "script/host/clr_host.h"
#include "script/host/guest_bridge.h"

namespace rx::script {
class PapyrusGuest;
}

namespace rx::script::host {

// The engine-facing handle to the managed scripting world. It wires the two
// directions of the boundary: it builds the inbound bridge over a Papyrus guest,
// boots the .NET host, and the managed entrypoint hands back the outbound
// callbacks this then drives every frame.
//
// Like the renderer's optional backends, it degrades gracefully: if .NET or the
// assembly is missing, Boot() returns false and available() stays false, so the
// engine runs without managed scripting instead of failing. The guest must
// outlive the host.
class ManagedHost {
 public:
  ManagedHost() = default;
  ~ManagedHost();

  ManagedHost(const ManagedHost&) = delete;
  ManagedHost& operator=(const ManagedHost&) = delete;

  // Registers a content domain the managed world can reach, before Boot. Call
  // once per loaded game, primary (rendered) game first: it becomes the default
  // bridge and domains[0]. `name` is the game's display name; loader pulls a
  // script (and its ancestor chain) from that domain's VFS by name for the
  // bridge's load_script (pass {} to load only already-present types). The guest
  // must outlive the host.
  void AddDomain(std::string name, PapyrusGuest& guest,
                 std::function<bool(const std::string&)> loader);

  // Boots the managed world over the registered domains. The paths locate the
  // .NET runtime and the Recreation.Scripting assembly (see ClrHost::Initialize).
  // Returns false (and leaves available() false) when no domain was registered,
  // or the runtime or assembly is unavailable.
  bool Boot(const std::string& dotnet_root, const std::string& runtime_config,
            const std::string& assembly);

  // Hands the managed world the ultragui widget-operation table (a
  // rx::ugui_cs::WidgetOps*, opaque here) so its UI handlers can read and mutate
  // live widgets. Call before Boot; passed through the handshake. Null disables
  // managed widget access (handlers still fire, but cannot touch widgets).
  void SetUiWidgetOps(const void* ops) { ui_widget_ops_ = ops; }

  // Routes a ultragui handler into the managed UI layer (the runtime installs
  // this as the backend's dispatch callback). Returns 1 if a handler claimed it,
  // 0 when unavailable or the managed side declined UI scripting.
  std::int32_t DispatchUi(const char* func_name, std::uint64_t widget);

  // Hands the managed world the multiplayer RPC surface (emit/subscribe), so mod
  // scripts drive the session. Call before Boot; passed through the handshake.
  void SetRpcBridge(const RpcBridge& rpc) { rpc_bridge_ = rpc; }

  // Sets the process role (0 server, 1 client, 2 standalone) the managed world
  // filters mods by. Call before Boot; passed through the handshake.
  void SetRealm(std::int32_t realm) { realm_ = realm; }

  // Delivers an inbound session RPC to the managed world. Safe to call from any
  // engine thread; the callback runs on the guest thread. No-op when unavailable
  // or the managed side declined RPC.
  void DispatchRpc(const char* name, std::int32_t sender, std::int32_t from_server,
                   const ApiValue* args, std::int32_t argc);

  bool available() const { return available_; }

  // Advances the managed world one frame. No-op when unavailable.
  void Tick(float dt);
  // Delivers an engine event to the managed event bus. Safe to call from any
  // engine thread; the callback runs on the guest thread. No-op when unavailable.
  void PublishEvent(const ManagedEvent& event);

  // Enqueues an event from any thread (the guest thread raises most of them).
  // The main loop later drains them with DrainEvents, which is where they reach
  // managed code, so engine threads never call into the CLR directly.
  void QueueEvent(const ManagedEvent& event);
  // Publishes every queued event. Host (main) thread only.
  void DrainEvents();

  // Tears the managed world down. Safe to call more than once.
  void Shutdown();

 private:
  // One registered domain. Heap allocated so its BridgeContext address (which the
  // ScriptBridge::ctx points at) stays stable as more domains are added.
  struct Domain {
    std::string name;
    BridgeContext ctx;
    ScriptBridge bridge{};
  };

  // Runs a managed callback on the guest thread and blocks until it returns, so
  // every call it makes back through the bridge takes the guest's same-thread
  // fast path (PapyrusGuest::Dispatch) instead of a per-call cross-thread hop.
  // This is what keeps the whole managed world on one thread (the SDK assumes
  // single-threaded execution): tick, events, UI, RPC and shutdown all funnel
  // through here onto the guest thread. Falls back to running inline when there
  // is no guest yet or it is not running (early init / teardown).
  void RunManaged(const std::function<void()>& fn);

  ClrHost clr_;
  PapyrusGuest* primary_guest_ = nullptr;  // domains_[0]'s guest; the managed thread
  std::vector<std::unique_ptr<Domain>> domains_;
  std::vector<DomainBridge> domain_table_;  // handshake view: borrows name/bridge
  HostHandshake handshake_{};
  const void* ui_widget_ops_ = nullptr;  // rx::ugui_cs::WidgetOps*, set before Boot
  RpcBridge rpc_bridge_{};               // multiplayer RPC surface, set before Boot
  std::int32_t realm_ = 2;               // process role, standalone until set
  bool available_ = false;

  std::mutex event_mutex_;
  std::vector<ManagedEvent> pending_events_;
};

}  // namespace rx::script::host

#endif  // RECREATION_SCRIPT_HOST_MANAGED_HOST_H_

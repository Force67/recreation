#ifndef RECREATION_SCRIPT_HOST_GUEST_BRIDGE_H_
#define RECREATION_SCRIPT_HOST_GUEST_BRIDGE_H_

#include <functional>
#include <string>

#include "script/host/bridge.h"

namespace rx::script {
class PapyrusGuest;
}

namespace rx::script::host {

// What a ScriptBridge needs from the engine. The guest serves every dispatch,
// property and tick call; the loader pulls a script (and its ancestor chain)
// from the asset VFS by name, which the guest alone cannot do. The runtime owns
// this object and keeps it alive for as long as managed code may call the
// bridge; tests that drive the guest directly leave loader empty.
struct BridgeContext {
  PapyrusGuest* guest = nullptr;
  // Returns true if the named script type is available after the call. Empty
  // means "no VFS": load_script then only reports already-loaded types.
  std::function<bool(const std::string&)> loader;
};

// Builds a ScriptBridge whose function pointers route through ctx. The table's
// ctx field points at ctx, so ctx must outlive any managed use of the bridge.
// The dispatch functions run on the guest thread and block until it replies, so
// managed code drives the guest without ever touching the VM directly.
ScriptBridge MakeScriptBridge(BridgeContext& ctx);

}  // namespace rx::script::host

#endif  // RECREATION_SCRIPT_HOST_GUEST_BRIDGE_H_

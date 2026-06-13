#ifndef RECREATION_SCRIPT_HOST_GUEST_BRIDGE_H_
#define RECREATION_SCRIPT_HOST_GUEST_BRIDGE_H_

#include "script/host/bridge.h"

namespace rec::script {
class PapyrusGuest;
}

namespace rec::script::host {

// Builds a GuestBridge backed by a PapyrusGuest. The returned table's function
// pointers route through the guest's thread-safe API, so managed code on the
// host thread drives the guest without ever touching the VM directly. The guest
// must outlive any managed use of the bridge.
GuestBridge MakeGuestBridge(PapyrusGuest& guest);

}  // namespace rec::script::host

#endif  // RECREATION_SCRIPT_HOST_GUEST_BRIDGE_H_

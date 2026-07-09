#ifndef RECREATION_SCRIPT_HOST_MANAGED_GC_PROFILE_H_
#define RECREATION_SCRIPT_HOST_MANAGED_GC_PROFILE_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rx::script::host {

// The .NET runtime's garbage collector and heap behaviour are configurable per
// process, and the right choice depends on where the game runs. The managed
// world is embedded in-process alongside the renderer and engine, so on a
// memory-constrained target (a handheld / Android build) an unbounded managed
// heap can starve the engine; on a desktop with RAM to spare, keeping freed
// segments committed trades a little memory for steadier frame times; on a
// dedicated server the trade-offs differ again.
//
// These functions turn "which platform + role am I?" into the hostfxr runtime
// properties ClrHost applies before the runtime starts (see
// ClrHost::Initialize). Kept as a small pure function so it is easy to test and
// to see exactly which knobs each profile sets.

using GcProperty = std::pair<std::string, std::string>;

// Picks the profile name for this build and role: "server" for a dedicated host
// (realm 0), "constrained" on a memory-limited platform, "desktop" otherwise.
// RECREATION_MANAGED_GC overrides the choice (for ops and testing).
std::string ResolveGcProfileName(std::int32_t realm);

// Builds the runtime properties for a named profile ("server" | "constrained" |
// "desktop"; anything else falls back to "desktop"). Per-knob environment
// overrides are applied last: RECREATION_MANAGED_GC_HEAP_PERCENT,
// RECREATION_MANAGED_GC_CONSERVE (0-9), RECREATION_MANAGED_GC_SERVER (0/1).
std::vector<GcProperty> ManagedGcProfile(const std::string& name);

}  // namespace rx::script::host

#endif  // RECREATION_SCRIPT_HOST_MANAGED_GC_PROFILE_H_

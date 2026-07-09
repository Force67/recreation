#include "script/host/managed_gc_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace rx::script::host {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

const char* Env(const char* name) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : nullptr;
}

bool EnvTruthy(const char* name) {
  const char* v = Env(name);
  if (!v) return false;
  const std::string s = Lower(v);
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

// Sets a property, replacing any existing entry with the same name so env
// overrides win over the profile default.
void Set(std::vector<GcProperty>& props, const std::string& name, const std::string& value) {
  for (GcProperty& p : props)
    if (p.first == name) {
      p.second = value;
      return;
    }
  props.push_back({name, value});
}

}  // namespace

std::string ResolveGcProfileName(std::int32_t realm) {
  if (const char* forced = Env("RECREATION_MANAGED_GC")) {
    const std::string name = Lower(forced);
    if (name == "server" || name == "constrained" || name == "desktop") return name;
  }
  if (realm == 0) return "server";  // dedicated host
#if defined(__ANDROID__)
  return "constrained";
#else
  return "desktop";
#endif
}

std::vector<GcProperty> ManagedGcProfile(const std::string& name) {
  std::vector<GcProperty> props;
  // Background (concurrent) GC on every profile: it collects gen2 off the mutator
  // thread, so the guest thread the managed world runs on does not stall a frame
  // on a full collection.
  Set(props, "System.GC.Concurrent", "true");

  if (name == "constrained") {
    // Cap the managed heap to a fraction of available memory so C# cannot crowd
    // out the engine, and let the GC give memory back aggressively.
    Set(props, "System.GC.HeapHardLimitPercent", "25");
    Set(props, "System.GC.ConserveMemory", "5");
    Set(props, "System.GC.RetainVM", "false");
  } else if (name == "server") {
    // Dedicated host: workstation GC by default (server GC's per-core heaps cost
    // real RAM; opt in with RECREATION_MANAGED_GC_SERVER once a load test shows a
    // throughput win). Retain freed segments to smooth out steady-state churn.
    Set(props, "System.GC.RetainVM", "true");
  } else {
    // desktop (default): RAM to spare, so keep freed segments committed for
    // steadier frame times.
    Set(props, "System.GC.RetainVM", "true");
  }

  // Per-knob environment overrides, applied last so they win.
  if (const char* pct = Env("RECREATION_MANAGED_GC_HEAP_PERCENT"))
    Set(props, "System.GC.HeapHardLimitPercent", pct);
  if (const char* conserve = Env("RECREATION_MANAGED_GC_CONSERVE"))
    Set(props, "System.GC.ConserveMemory", conserve);
  if (EnvTruthy("RECREATION_MANAGED_GC_SERVER")) Set(props, "System.GC.Server", "true");

  return props;
}

}  // namespace rx::script::host

#ifndef RECREATION_SCRIPT_HOST_CLR_HOST_H_
#define RECREATION_SCRIPT_HOST_CLR_HOST_H_

#include <string>

namespace rec::script::host {

// Hosts the .NET CoreCLR runtime in process and resolves a managed entrypoint.
// This is the engine's scripting runtime: the "main" world users write C#
// against. It loads libhostfxr at runtime (never a build-time .NET dependency,
// matching how the renderer treats its optional backends), so a build without
// .NET still compiles and simply reports the host unavailable.
//
// The managed entrypoint is an [UnmanagedCallersOnly] static method. Invoke
// hands it a pointer (the GuestBridge), which is how managed code calls into
// the Papyrus guest.
class ClrHost {
 public:
  ClrHost() = default;
  ~ClrHost();

  ClrHost(const ClrHost&) = delete;
  ClrHost& operator=(const ClrHost&) = delete;

  // Boots the runtime from runtime_config_path (the assembly's .runtimeconfig
  // .json) and resolves type_name::method_name from assembly_path.
  //   dotnet_root  : a .NET root containing host/fxr/<ver>/libhostfxr.so. Empty
  //                  falls back to the DOTNET_ROOT environment variable.
  //   type_name    : assembly-qualified, e.g. "Recreation.ScriptHost, Recreation.Scripting".
  //   method_name  : the [UnmanagedCallersOnly] static method.
  // Returns false (and leaves available() false) if .NET is not present or the
  // entrypoint cannot be resolved.
  bool Initialize(const std::string& dotnet_root, const std::string& runtime_config_path,
                  const std::string& assembly_path, const std::string& type_name,
                  const std::string& method_name);

  bool available() const { return entry_ != nullptr; }

  // Calls the managed entrypoint, forwarding arg (the GuestBridge*). Returns the
  // managed return value, or -1 if the host is unavailable.
  int Invoke(void* arg);

  void Shutdown();

 private:
  void* library_ = nullptr;       // dlopen handle for libhostfxr
  void* host_handle_ = nullptr;   // hostfxr_handle
  void* close_fn_ = nullptr;      // hostfxr_close_fn
  int (*entry_)(void*) = nullptr;  // resolved managed entrypoint
};

}  // namespace rec::script::host

#endif  // RECREATION_SCRIPT_HOST_CLR_HOST_H_

#include "script/host/clr_host.h"

#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>

#include "core/log.h"

namespace rec::script::host {
namespace {

// Minimal slice of the public .NET hosting ABI (hostfxr.h / coreclr_delegates.h).
// On Linux char_t is char and the calltypes are empty, so these typedefs are
// the whole contract; vendoring the headers would add nothing.
using char_t = char;
using hostfxr_handle = void*;
constexpr int kHdtLoadAssemblyAndGetFunctionPointer = 5;  // hostfxr_delegate_type
const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

using init_for_runtime_config_fn = int (*)(const char_t*, const void*, hostfxr_handle*);
using get_runtime_delegate_fn = int (*)(hostfxr_handle, int, void**);
using close_fn = int (*)(hostfxr_handle);
using load_assembly_and_get_function_pointer_fn = int (*)(const char_t*, const char_t*,
                                                          const char_t*, const char_t*, void*,
                                                          void**);

// Picks the newest host/fxr/<version>/libhostfxr.so under a .NET root, choosing
// by version directory name (lexicographic, good enough for the x.y.z layout).
std::string FindHostFxr(const std::string& dotnet_root) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::string best_version;
  std::string best_path;
  for (const auto& entry : fs::directory_iterator(fs::path(dotnet_root) / "host" / "fxr", ec)) {
    if (!entry.is_directory()) continue;
    fs::path candidate = entry.path() / "libhostfxr.so";
    std::string version = entry.path().filename().string();
    if (fs::exists(candidate) && version > best_version) {
      best_version = version;
      best_path = candidate.string();
    }
  }
  return best_path;
}

}  // namespace

ClrHost::~ClrHost() { Shutdown(); }

bool ClrHost::Initialize(const std::string& dotnet_root, const std::string& runtime_config_path,
                         const std::string& assembly_path, const std::string& type_name,
                         const std::string& method_name) {
  std::string root = dotnet_root;
  if (root.empty()) {
    if (const char* env = std::getenv("DOTNET_ROOT")) root = env;
  }
  if (root.empty()) {
    REC_INFO("clr: no .NET root (set DOTNET_ROOT), managed scripting disabled");
    return false;
  }

  std::string fxr_path = FindHostFxr(root);
  if (fxr_path.empty()) {
    REC_WARN("clr: libhostfxr.so not found under {}, managed scripting disabled", root);
    return false;
  }

  library_ = dlopen(fxr_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
  if (!library_) {
    REC_WARN("clr: dlopen {} failed: {}", fxr_path, dlerror());
    return false;
  }

  auto init = reinterpret_cast<init_for_runtime_config_fn>(dlsym(library_, "hostfxr_initialize_for_runtime_config"));
  auto get_delegate = reinterpret_cast<get_runtime_delegate_fn>(dlsym(library_, "hostfxr_get_runtime_delegate"));
  close_fn_ = dlsym(library_, "hostfxr_close");
  if (!init || !get_delegate || !close_fn_) {
    REC_WARN("clr: hostfxr symbols missing");
    return false;
  }

  hostfxr_handle handle = nullptr;
  int rc = init(runtime_config_path.c_str(), nullptr, &handle);
  if (rc < 0 || !handle) {
    REC_WARN("clr: initialize_for_runtime_config({}) failed: 0x{:x}", runtime_config_path,
             static_cast<unsigned>(rc));
    return false;
  }
  host_handle_ = handle;

  void* load_ptr = nullptr;
  rc = get_delegate(handle, kHdtLoadAssemblyAndGetFunctionPointer, &load_ptr);
  if (rc != 0 || !load_ptr) {
    REC_WARN("clr: get_runtime_delegate failed: 0x{:x}", static_cast<unsigned>(rc));
    return false;
  }
  auto load = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(load_ptr);

  void* entry = nullptr;
  rc = load(assembly_path.c_str(), type_name.c_str(), method_name.c_str(), kUnmanagedCallersOnly,
            nullptr, &entry);
  if (rc != 0 || !entry) {
    REC_WARN("clr: resolving {}::{} failed: 0x{:x}", type_name, method_name,
             static_cast<unsigned>(rc));
    return false;
  }
  entry_ = reinterpret_cast<int (*)(void*)>(entry);
  REC_INFO("clr: .NET runtime hosted, entrypoint {}::{} ready", type_name, method_name);
  return true;
}

int ClrHost::Invoke(void* arg) { return entry_ ? entry_(arg) : -1; }

void ClrHost::Shutdown() {
  if (host_handle_ && close_fn_) reinterpret_cast<close_fn>(close_fn_)(host_handle_);
  host_handle_ = nullptr;
  entry_ = nullptr;
  // libhostfxr is intentionally left loaded: the CLR cannot be re-hosted in a
  // process, so there is nothing to gain from unloading it.
}

}  // namespace rec::script::host

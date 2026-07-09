#include "script/host/clr_host.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/log.h"

namespace rx::script::host {
namespace {

// Platform shim over the OS dynamic loader so the .NET host code below stays a
// single implementation. The hostfxr ABI is UTF-16 on Windows (char_t ==
// wchar_t, hostfxr.dll) and UTF-8 on POSIX (char_t == char, libhostfxr.so).
#if defined(_WIN32)
using char_t = wchar_t;
constexpr const char* kHostFxrName = "hostfxr.dll";

void* OsLoadLibrary(const char* utf8_path) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, nullptr, 0);
  std::wstring wide(n > 0 ? n : 1, L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide.data(), n);
  return reinterpret_cast<void*>(::LoadLibraryW(wide.c_str()));
}
void* OsGetSymbol(void* lib, const char* name) {
  return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
}
std::string OsLoadError() { return "error " + std::to_string(::GetLastError()); }

// hostfxr's UTF-16 entry points need the UTF-8 std::strings widened; the
// returned temporary outlives the call expression it is passed into.
std::wstring ToCharT(const std::string& s) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring wide(n > 1 ? n - 1 : 0, L'\0');
  if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, wide.data(), n);
  return wide;
}
#else
using char_t = char;
constexpr const char* kHostFxrName = "libhostfxr.so";

void* OsLoadLibrary(const char* path) { return ::dlopen(path, RTLD_LAZY | RTLD_GLOBAL); }
void* OsGetSymbol(void* lib, const char* name) { return ::dlsym(lib, name); }
std::string OsLoadError() {
  const char* e = ::dlerror();
  return e ? e : "unknown error";
}
const std::string& ToCharT(const std::string& s) { return s; }
#endif

using hostfxr_handle = void*;
constexpr int kHdtLoadAssemblyAndGetFunctionPointer = 5;  // hostfxr_delegate_type
const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

using init_for_runtime_config_fn = int (*)(const char_t*, const void*, hostfxr_handle*);
using get_runtime_delegate_fn = int (*)(hostfxr_handle, int, void**);
using set_runtime_property_fn = int (*)(hostfxr_handle, const char_t*, const char_t*);
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
    fs::path candidate = entry.path() / kHostFxrName;
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
                         const std::string& method_name,
                         const std::vector<std::pair<std::string, std::string>>& properties) {
  std::string root = dotnet_root;
  if (root.empty()) {
    if (const char* env = std::getenv("DOTNET_ROOT")) root = env;
  }
  if (root.empty()) {
    RX_INFO("clr: no .NET root (set DOTNET_ROOT), managed scripting disabled");
    return false;
  }

  std::string fxr_path = FindHostFxr(root);
  if (fxr_path.empty()) {
    RX_WARN("clr: libhostfxr.so not found under {}, managed scripting disabled", root);
    return false;
  }

  library_ = OsLoadLibrary(fxr_path.c_str());
  if (!library_) {
    RX_WARN("clr: loading {} failed: {}", fxr_path, OsLoadError());
    return false;
  }

  auto init = reinterpret_cast<init_for_runtime_config_fn>(OsGetSymbol(library_, "hostfxr_initialize_for_runtime_config"));
  auto get_delegate = reinterpret_cast<get_runtime_delegate_fn>(OsGetSymbol(library_, "hostfxr_get_runtime_delegate"));
  close_fn_ = OsGetSymbol(library_, "hostfxr_close");
  if (!init || !get_delegate || !close_fn_) {
    RX_WARN("clr: hostfxr symbols missing");
    return false;
  }

  hostfxr_handle handle = nullptr;
  int rc = init(ToCharT(runtime_config_path).c_str(), nullptr, &handle);
  if (rc < 0 || !handle) {
    RX_WARN("clr: initialize_for_runtime_config({}) failed: 0x{:x}", runtime_config_path,
             static_cast<unsigned>(rc));
    return false;
  }
  host_handle_ = handle;

  // Apply the runtime configuration properties (the per-platform GC/heap profile)
  // now, after initialize but before get_runtime_delegate starts the runtime:
  // GC mode and heap-limit knobs are read once at startup, so this is the only
  // window in which they take effect.
  if (!properties.empty()) {
    auto set_prop = reinterpret_cast<set_runtime_property_fn>(
        OsGetSymbol(library_, "hostfxr_set_runtime_property_value"));
    if (!set_prop) {
      RX_WARN("clr: hostfxr_set_runtime_property_value unavailable; GC profile not applied");
    } else {
      for (const auto& kv : properties) {
        const auto name = ToCharT(kv.first);
        const auto value = ToCharT(kv.second);
        const int prc = set_prop(handle, name.c_str(), value.c_str());
        if (prc != 0)
          RX_WARN("clr: runtime property {}={} rejected: 0x{:x}", kv.first, kv.second,
                   static_cast<unsigned>(prc));
        else
          RX_INFO("clr: runtime property {}={}", kv.first, kv.second);
      }
    }
  }

  void* load_ptr = nullptr;
  rc = get_delegate(handle, kHdtLoadAssemblyAndGetFunctionPointer, &load_ptr);
  if (rc != 0 || !load_ptr) {
    RX_WARN("clr: get_runtime_delegate failed: 0x{:x}", static_cast<unsigned>(rc));
    return false;
  }
  auto load = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(load_ptr);

  void* entry = nullptr;
  rc = load(ToCharT(assembly_path).c_str(), ToCharT(type_name).c_str(),
            ToCharT(method_name).c_str(), kUnmanagedCallersOnly, nullptr, &entry);
  if (rc != 0 || !entry) {
    RX_WARN("clr: resolving {}::{} failed: 0x{:x}", type_name, method_name,
             static_cast<unsigned>(rc));
    return false;
  }
  entry_ = reinterpret_cast<int (*)(void*)>(entry);
  RX_INFO("clr: .NET runtime hosted, entrypoint {}::{} ready", type_name, method_name);
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

}  // namespace rx::script::host

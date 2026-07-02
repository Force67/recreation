#include "render/post/ngx_context.h"

#include <string>

#include <base/option.h>

#include "core/log.h"
#include "render/rhi/device.h"
// Vulkan escape hatch: NGX speaks raw Vulkan. Also pulls volk
// (VK_NO_PROTOTYPES) before the ngx vk header.
#include "render/rhi/vulkan_interop.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>

namespace rec::render::ngx {
namespace {

// Arbitrary application id for the CUSTOM engine path; NGX validates it is a
// well formed UUID (8-4-4-4-12 hex), it does not need to be registered.
constexpr const char* kProjectId = "8d4a1f60-3c2e-4b8a-9f17-c4035c19df04";

// RECREATION_DLSS_LIB_DIR overrides the baked-in snippet dir; read straight
// from the environment.
base::Option<const char*> DlssLibDir{"dlss.lib.dir", nullptr, "RECREATION_DLSS_LIB_DIR"};

void NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string line(message ? message : "");
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (!line.empty()) REC_WARN("ngx: {}", line);
}

std::wstring ToWide(const char* s) {
  std::wstring w;
  for (; s && *s; ++s) w.push_back(static_cast<wchar_t>(*s));
  return w;
}

struct Context {
  int refcount = 0;
  bool initialized = false;
  VkDevice vk_device = VK_NULL_HANDLE;
  NVSDK_NGX_Parameter* capability = nullptr;
  std::wstring snippet_dir;
  const wchar_t* snippet_path = nullptr;
};
Context g_context;

}  // namespace

bool Acquire(Device& device) {
  if (g_context.initialized) {
    ++g_context.refcount;
    return true;
  }

  VulkanHandles h = GetVulkanHandles(device);
  if (h.device == VK_NULL_HANDLE) {
    REC_WARN("ngx: requires the vulkan backend");
    return false;
  }

  // PathListInfo tells NGX where the DLSS inference snippets (.so) live; the
  // SDK lib dir is baked in at build time by third_party/dlss.cmake, with an
  // env override so a driver-bundled or dev snippet can be pointed at without
  // a rebuild. NGX additionally searches next to the executable.
  const char* lib_dir = DlssLibDir.get();
  g_context.snippet_dir = ToWide(lib_dir && *lib_dir ? lib_dir : RECREATION_DLSS_LIB_DIR);
  g_context.snippet_path = g_context.snippet_dir.c_str();
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo.Path = &g_context.snippet_path;
  common.PathListInfo.Length = 1;
  common.LoggingInfo.LoggingCallback = NgxLog;
  common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

  NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
      kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", L"/tmp", h.instance, h.physical_device,
      h.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common, NVSDK_NGX_Version_API);
  if (r != NVSDK_NGX_Result_Success) {
    REC_ERROR("ngx: init failed ({:#x})", static_cast<u32>(r));
    return false;
  }

  if (NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_context.capability) !=
          NVSDK_NGX_Result_Success ||
      !g_context.capability) {
    REC_ERROR("ngx: capability parameters unavailable");
    NVSDK_NGX_VULKAN_Shutdown1(h.device);
    return false;
  }

  g_context.vk_device = h.device;
  g_context.initialized = true;
  g_context.refcount = 1;
  return true;
}

void Release() {
  if (!g_context.initialized) return;
  if (--g_context.refcount > 0) return;
  // Deliberately NOT calling NVSDK_NGX_VULKAN_Shutdown1: the driver's aarch64
  // implementation crashes (observed on GB10, 580.159.03 - a pass-through
  // wrapper per the SDK disassembly, so the fault is driver-internal). NGX
  // stays initialized for the process lifetime; the OS reclaims it at exit.
  // Refcounting is kept so a future driver fix only needs this comment gone.
  g_context.refcount = 0;
}

NVSDK_NGX_Parameter* Capability() {
  return g_context.initialized ? g_context.capability : nullptr;
}

}  // namespace rec::render::ngx

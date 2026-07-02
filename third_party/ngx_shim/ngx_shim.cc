// aarch64 NGX link shim. NVIDIA's DLSS SDK ships libnvsdk_ngx.a only for
// x86_64, but the driver's libnvidia-ngx.so.1 (which that static lib dlopens
// anyway) exports the entire SDK-level C ABI on every architecture, with two
// exceptions handled here. Symbols resolve lazily out of the driver library,
// exactly like the official wrapper does.
//
// The one non-trivial mapping was recovered from the official x86_64 wrapper's
// disassembly: the public NVSDK_NGX_VULKAN_Init_with_ProjectID(..., info,
// sdk_version) forwards to the driver's NVSDK_NGX_VULKAN_Init_ProjectID_Ext
// with the LAST TWO ARGUMENTS SWAPPED (..., sdk_version, info), falling back
// to NVSDK_NGX_VULKAN_Init_ProjectID (no GIPA/GDPA) when Ext is absent.

#include <dlfcn.h>

#include <initializer_list>

#include <vulkan/vulkan.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>

namespace {

void* NgxDriver() {
  static void* handle = [] {
    for (const char* name : {"libnvidia-ngx.so.1", "libnvidia-ngx.so"}) {
      if (void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL)) return h;
    }
    return static_cast<void*>(nullptr);
  }();
  return handle;
}

template <typename Fn>
Fn Sym(const char* name) {
  void* driver = NgxDriver();
  return driver ? reinterpret_cast<Fn>(dlsym(driver, name)) : nullptr;
}

}  // namespace

extern "C" NVSDK_NGX_Result NVSDK_NGX_VULKAN_Init_with_ProjectID(
    const char* project_id, NVSDK_NGX_EngineType engine_type, const char* engine_version,
    const wchar_t* application_data_path, VkInstance instance, VkPhysicalDevice physical_device,
    VkDevice device, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
    const NVSDK_NGX_FeatureCommonInfo* feature_info, NVSDK_NGX_Version sdk_version) {
  using InitExt = NVSDK_NGX_Result (*)(const char*, NVSDK_NGX_EngineType, const char*,
                                       const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                                       PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                       NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
  if (auto fn = Sym<InitExt>("NVSDK_NGX_VULKAN_Init_ProjectID_Ext")) {
    return fn(project_id, engine_type, engine_version, application_data_path, instance,
              physical_device, device, gipa, gdpa, sdk_version, feature_info);
  }
  using Init = NVSDK_NGX_Result (*)(const char*, NVSDK_NGX_EngineType, const char*,
                                    const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                                    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
  if (auto fn = Sym<Init>("NVSDK_NGX_VULKAN_Init_ProjectID")) {
    return fn(project_id, engine_type, engine_version, application_data_path, instance,
              physical_device, device, sdk_version, feature_info);
  }
  return NVSDK_NGX_Result_FAIL_FeatureNotFound;
}

// Every other public entry point exists under its own name in the driver
// library; forward one-to-one.
#define NGX_FORWARD(ret, name, params, args)              \
  extern "C" ret name params {                            \
    using Fn = ret(*) params;                             \
    static Fn fn = Sym<Fn>(#name);                        \
    if (!fn) return NVSDK_NGX_Result_FAIL_FeatureNotFound; \
    return fn args;                                       \
  }

NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_RequiredExtensions,
            (unsigned int* inst_count, const char*** inst_exts, unsigned int* device_count,
             const char*** device_exts),
            (inst_count, inst_exts, device_count, device_exts))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements,
            (const NVSDK_NGX_FeatureDiscoveryInfo* info, uint32_t* count,
             VkExtensionProperties** exts),
            (info, count, exts))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements,
            (VkInstance instance, VkPhysicalDevice physical_device,
             const NVSDK_NGX_FeatureDiscoveryInfo* info, uint32_t* count,
             VkExtensionProperties** exts),
            (instance, physical_device, info, count, exts))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_Shutdown1, (VkDevice device), (device))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_GetCapabilityParameters,
            (NVSDK_NGX_Parameter** params), (params))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_AllocateParameters,
            (NVSDK_NGX_Parameter** params), (params))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_DestroyParameters,
            (NVSDK_NGX_Parameter* params), (params))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_GetScratchBufferSize,
            (NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* params, size_t* size),
            (feature, params, size))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_CreateFeature1,
            (VkDevice device, VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
             NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle),
            (device, cmd, feature, params, handle))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_CreateFeature,
            (VkCommandBuffer cmd, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* params,
             NVSDK_NGX_Handle** handle),
            (cmd, feature, params, handle))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_ReleaseFeature, (NVSDK_NGX_Handle* handle),
            (handle))
NGX_FORWARD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_EvaluateFeature,
            (VkCommandBuffer cmd, const NVSDK_NGX_Handle* handle,
             const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback progress),
            (cmd, handle, params, progress))

// The parameter Get/Set C helpers are SDK-side conveniences over the
// NVSDK_NGX_Parameter C++ virtual interface (the objects come from the
// driver); implement them directly.
extern "C" void NVSDK_NGX_Parameter_SetULL(NVSDK_NGX_Parameter* p, const char* name,
                                           unsigned long long v) { if (p) p->Set(name, v); }
extern "C" void NVSDK_NGX_Parameter_SetD(NVSDK_NGX_Parameter* p, const char* name, double v) {
  if (p) p->Set(name, v);
}
extern "C" void NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* p, const char* name,
                                          unsigned int v) { if (p) p->Set(name, v); }
extern "C" void NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter* p, const char* name, int v) {
  if (p) p->Set(name, v);
}
extern "C" void NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter* p, const char* name, float v) {
  if (p) p->Set(name, v);
}
extern "C" void NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter* p, const char* name,
                                                   void* v) { if (p) p->Set(name, v); }
#define NGX_PARAM_GET(suffix, type)                                                       \
  extern "C" NVSDK_NGX_Result NVSDK_NGX_Parameter_Get##suffix(NVSDK_NGX_Parameter* p,     \
                                                              const char* name, type* v) { \
    return p ? p->Get(name, v) : NVSDK_NGX_Result_FAIL_InvalidParameter;                  \
  }
NGX_PARAM_GET(ULL, unsigned long long)
NGX_PARAM_GET(D, double)
NGX_PARAM_GET(UI, unsigned int)
NGX_PARAM_GET(I, int)
NGX_PARAM_GET(F, float)
NGX_PARAM_GET(VoidPointer, void*)

// The _C variant only differs in the progress-callback signature; the engine
// never passes a callback, and wrapping one statelessly is impossible, so a
// non-null callback is deliberately dropped.
extern "C" NVSDK_NGX_Result NVSDK_NGX_VULKAN_EvaluateFeature_C(
    VkCommandBuffer cmd, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
    PFN_NVSDK_NGX_ProgressCallback_C /*callback*/) {
  return NVSDK_NGX_VULKAN_EvaluateFeature(cmd, handle, params, nullptr);
}

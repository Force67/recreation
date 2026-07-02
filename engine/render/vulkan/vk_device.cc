#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

#include <base/containers/vector.h>

#include "core/log.h"
#include "render/vulkan/vk_backend.h"

#if defined(RECREATION_HAS_DLSS)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#endif

namespace rec::render::vk {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    REC_ERROR("vulkan: {}", data->pMessage);
  } else {
    REC_WARN("vulkan: {}", data->pMessage);
  }
  return VK_FALSE;
}

bool HasValidationLayer() {
  u32 count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  base::Vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::ranges::any_of(layers, [](const auto& layer) {
    return std::strcmp(layer.layerName, kValidationLayer) == 0;
  });
}

base::Vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice physical) {
  u32 count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  base::Vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
  return extensions;
}

bool HasExtension(const base::Vector<VkExtensionProperties>& available, const char* name) {
  return std::ranges::any_of(
      available, [name](const auto& ext) { return std::strcmp(ext.extensionName, name) == 0; });
}

bool HasInstanceExtension(const char* name) {
  u32 count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  base::Vector<VkExtensionProperties> extensions(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
  return HasExtension(extensions, name);
}

// Graphics family that can also present. Async compute and transfer queues
// come later, one universal queue is enough for bringup.
int FindGraphicsFamily(VkPhysicalDevice physical, VkSurfaceKHR surface) {
  u32 count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  base::Vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
  for (u32 i = 0; i < count; ++i) {
    if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
    if (present) return static_cast<int>(i);
  }
  return -1;
}

VkPhysicalDevice PickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface) {
  u32 count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  base::Vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());

  VkPhysicalDevice best = VK_NULL_HANDLE;
  int best_score = -1;
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(candidate, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) continue;
    if (FindGraphicsFamily(candidate, surface) < 0) continue;
    if (!HasExtension(DeviceExtensions(candidate), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
    if (score > best_score) {
      best_score = score;
      best = candidate;
    }
  }
  return best;
}

VkShaderModule CreateModule(VkDevice device, const ShaderBlob& blob) {
  if (!blob.valid() || blob.format != ShaderFormat::kSpirv) return VK_NULL_HANDLE;
  // The embedded arrays are byte aligned, spirv wants words, so copy.
  base::Vector<u32> words((blob.size + 3) / 4);
  std::memcpy(words.data(), blob.data, blob.size);
  VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = words.size() * 4;
  info.pCode = words.data();
  VkShaderModule module = VK_NULL_HANDLE;
  vkCreateShaderModule(device, &info, nullptr, &module);
  return module;
}

u64 HashBytes(const void* data, size_t size, u64 seed = 1469598103934665603ull) {
  const u8* bytes = static_cast<const u8*>(data);
  u64 hash = seed;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 HashLayoutDesc(const BindingLayoutDesc& desc) {
  u64 hash = HashBytes(&desc.stages, sizeof(desc.stages));
  hash = HashBytes(&desc.update_after_bind, sizeof(bool), hash);
  for (const BindingSlot& slot : desc.slots) hash = HashBytes(&slot, sizeof(slot), hash);
  return hash;
}

struct BlendConfig {
  bool enable;
  VkBlendFactor src_color, dst_color, src_alpha, dst_alpha;
};

BlendConfig BlendConfigOf(BlendMode mode) {
  switch (mode) {
    case BlendMode::kOpaque:
      return {false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
              VK_BLEND_FACTOR_ZERO};
    case BlendMode::kAlpha:
      return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
              VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    case BlendMode::kPremultiplied:
      return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
              VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    case BlendMode::kAdditive:
      return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
              VK_BLEND_FACTOR_ONE};
    case BlendMode::kMultiply:
      return {true, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ZERO,
              VK_BLEND_FACTOR_SRC_ALPHA};
    case BlendMode::kWboitAccum:
      return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
              VK_BLEND_FACTOR_ONE};
    case BlendMode::kWboitReveal:
      return {true, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
              VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
  }
  return {false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
          VK_BLEND_FACTOR_ZERO};
}

// Transient descriptor pool sizing, per frame slot: generous multiples of the
// busiest frame observed, cheap in host memory.
constexpr u32 kTransientMaxSets = 512;
constexpr VkDescriptorPoolSize kTransientPoolSizes[] = {
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024},
    {VK_DESCRIPTOR_TYPE_SAMPLER, 256},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
    {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 64},
};

VkDescriptorPool CreateTransientPool(VkDevice device) {
  VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.maxSets = kTransientMaxSets;
  info.poolSizeCount = static_cast<u32>(std::size(kTransientPoolSizes));
  info.pPoolSizes = kTransientPoolSizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  vkCreateDescriptorPool(device, &info, nullptr, &pool);
  return pool;
}

}  // namespace

std::unique_ptr<Device> VulkanDevice::Create(const DeviceDesc& desc, Window& window) {
  auto device = std::unique_ptr<VulkanDevice>(new VulkanDevice());

  auto surface_extensions = window.vulkan_instance_extensions();
  if (surface_extensions.empty()) {
    REC_WARN("window cannot present, vulkan backend unavailable");
    return nullptr;
  }
  if (volkInitialize() != VK_SUCCESS) {
    REC_WARN("no vulkan loader on this system, vulkan backend unavailable");
    return nullptr;
  }
  if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
    REC_ERROR("vulkan 1.3 required, loader reports {}.{}",
              VK_API_VERSION_MAJOR(volkGetInstanceVersion()),
              VK_API_VERSION_MINOR(volkGetInstanceVersion()));
    return nullptr;
  }

  base::Vector<const char*> instance_extensions;
  instance_extensions.assign(surface_extensions.begin(), surface_extensions.end());
  base::Vector<const char*> layers;
  bool validation = desc.enable_validation && HasValidationLayer();
  if (validation) {
    layers.push_back(kValidationLayer);
  } else if (desc.enable_validation) {
    REC_WARN("validation requested but layer not installed");
  }
  // Debug utils carries the queue/command-buffer labels the GPU profiler emits
  // (and that capture tools group passes by), so enable it whenever present,
  // not just under validation.
  bool debug_utils = HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (debug_utils) instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  // MoltenVK exposes its GPU only as a portability-subset device, which the
  // loader hides unless the instance opts in via this extension and flag.
  VkInstanceCreateFlags instance_flags = 0;
#if defined(__APPLE__)
  if (HasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
#endif

#if defined(RECREATION_HAS_DLSS)
  // DLSS/NGX must have its instance and device extensions enabled at creation
  // time. The list is static (no init needed); the device half is stashed for
  // the device extension pass below.
  unsigned ngx_device_ext_count = 0;
  const char** ngx_device_exts = nullptr;
  {
    unsigned ngx_instance_ext_count = 0;
    const char** ngx_instance_exts = nullptr;
    if (NVSDK_NGX_VULKAN_RequiredExtensions(&ngx_instance_ext_count, &ngx_instance_exts,
                                            &ngx_device_ext_count, &ngx_device_exts) ==
        NVSDK_NGX_Result_Success) {
      for (unsigned i = 0; i < ngx_instance_ext_count; ++i) {
        const char* name = ngx_instance_exts[i];
        if (std::ranges::none_of(instance_extensions, [name](const char* e) {
              return std::strcmp(e, name) == 0;
            })) {
          instance_extensions.push_back(name);
        }
      }
    } else {
      REC_WARN("dlss: required extension query failed, dlss will be unavailable");
    }
  }
#endif

  VkApplicationInfo app_info{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "recreation";
  app_info.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.flags = instance_flags;
  instance_info.pApplicationInfo = &app_info;
  instance_info.enabledLayerCount = static_cast<u32>(layers.size());
  instance_info.ppEnabledLayerNames = layers.data();
  instance_info.enabledExtensionCount = static_cast<u32>(instance_extensions.size());
  instance_info.ppEnabledExtensionNames = instance_extensions.data();

  if (vkCreateInstance(&instance_info, nullptr, &device->instance_) != VK_SUCCESS) {
    REC_ERROR("vkCreateInstance failed");
    return nullptr;
  }
  volkLoadInstance(device->instance_);

  if (validation) {
    VkDebugUtilsMessengerCreateInfoEXT messenger_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = DebugCallback;
    vkCreateDebugUtilsMessengerEXT(device->instance_, &messenger_info, nullptr,
                                   &device->debug_messenger_);
  }

  if (!window.CreateVulkanSurface(device->instance_, &device->surface_)) {
    REC_ERROR("surface creation failed");
    return nullptr;
  }

  device->physical_device_ = PickPhysicalDevice(device->instance_, device->surface_);
  if (device->physical_device_ == VK_NULL_HANDLE) {
    REC_ERROR("no vulkan 1.3 capable gpu found");
    return nullptr;
  }

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(device->physical_device_, &props);
  device->caps_.backend = Backend::kVulkan;
  device->caps_.adapter_name = props.deviceName;
  device->caps_.api_version = props.apiVersion;
  device->caps_.integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

  VkPhysicalDeviceMemoryProperties mem;
  vkGetPhysicalDeviceMemoryProperties(device->physical_device_, &mem);
  for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
    if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      device->caps_.device_local_bytes += mem.memoryHeaps[i].size;
    }
  }
  device->graphics_family_ =
      static_cast<u32>(FindGraphicsFamily(device->physical_device_, device->surface_));

  auto available = DeviceExtensions(device->physical_device_);
  base::Vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

  // A portability-subset device (MoltenVK) must have this extension enabled when
  // it advertises it, otherwise vkCreateDevice fails. The name macro lives
  // behind VK_ENABLE_BETA_EXTENSIONS, so spell it literally to avoid pulling in
  // the beta headers just for the string.
#if defined(__APPLE__)
  constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";
  if (HasExtension(available, kPortabilitySubset)) {
    device_extensions.push_back(kPortabilitySubset);
  }
#endif

  bool want_raytracing = desc.request_raytracing &&
                         HasExtension(available, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                         HasExtension(available, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                         HasExtension(available, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  bool want_ray_query =
      want_raytracing && HasExtension(available, VK_KHR_RAY_QUERY_EXTENSION_NAME);
  bool want_mesh_shaders = HasExtension(available, VK_EXT_MESH_SHADER_EXTENSION_NAME);
  bool want_shading_rate = HasExtension(available, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
  // Some compute shaders (hi-z aware passes) declare quad derivatives; enable
  // the extension whenever the driver has it so their modules validate.
  bool want_compute_derivatives =
      HasExtension(available, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);

  // Feature query chain. Optional structs join the chain only when their
  // extension exists, then whatever the driver reports gets enabled.
  VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features f11{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features f12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features f13{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR ray_query{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceMeshShaderFeaturesEXT mesh{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading_rate{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR compute_derivatives{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};

  void** tail = &features.pNext;
  auto chain = [&tail](auto* node) {
    *tail = node;
    tail = &node->pNext;
  };
  chain(&f11);
  chain(&f12);
  chain(&f13);
  if (want_raytracing) {
    chain(&accel);
    chain(&rt_pipeline);
    device_extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    device_extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    device_extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  }
  if (want_ray_query) {
    chain(&ray_query);
    device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
  }
  if (want_mesh_shaders) {
    chain(&mesh);
    device_extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
  }
  if (want_shading_rate) {
    chain(&shading_rate);
    device_extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
  }
  if (want_compute_derivatives) {
    chain(&compute_derivatives);
    device_extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
  }

#if defined(RECREATION_HAS_DLSS)
  // Add NGX's device extensions that the gpu advertises (it is nvidia here, so
  // they should all be present). Missing ones just leave dlss unavailable.
  for (unsigned i = 0; i < ngx_device_ext_count; ++i) {
    const char* name = ngx_device_exts[i];
    if (!HasExtension(available, name)) continue;
    // Core-1.2 bufferDeviceAddress is enabled; the EXT variant NGX asks for
    // conflicts with it at device creation (VUID 04748). NGX works with the
    // core feature.
    if (!std::strcmp(name, "VK_EXT_buffer_device_address")) continue;
    if (std::ranges::any_of(device_extensions,
                            [name](const char* e) { return std::strcmp(e, name) == 0; })) {
      continue;
    }
    device_extensions.push_back(name);
  }
#endif

  vkGetPhysicalDeviceFeatures2(device->physical_device_, &features);

  if (!f13.dynamicRendering || !f13.synchronization2 || !f12.bufferDeviceAddress ||
      !f12.timelineSemaphore || !f12.descriptorIndexing) {
    REC_ERROR("{} lacks the required vulkan 1.3 baseline", device->caps_.adapter_name);
    return nullptr;
  }
  // Costs measurable bandwidth and the engine does its own bounds discipline.
  features.features.robustBufferAccess = VK_FALSE;

  device->caps_.raytracing = accel.accelerationStructure && rt_pipeline.rayTracingPipeline;
  device->caps_.ray_query = ray_query.rayQuery;
  device->caps_.mesh_shaders = mesh.meshShader;
  device->caps_.fragment_shading_rate =
      want_shading_rate && shading_rate.attachmentFragmentShadingRate;
  if (device->caps_.fragment_shading_rate) {
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsr_props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &fsr_props;
    vkGetPhysicalDeviceProperties2(device->physical_device_, &props2);
    device->caps_.shading_rate_texel =
        std::max(fsr_props.minFragmentShadingRateAttachmentTexelSize.width, 1u);
    device->caps_.shading_rate_max_size = std::max(fsr_props.maxFragmentSize.width, 1u);
  }
  device->caps_.fill_mode_non_solid = features.features.fillModeNonSolid;
  if (features.features.samplerAnisotropy) {
    device->caps_.max_anisotropy = props.limits.maxSamplerAnisotropy;
  }
  device->caps_.timestamp_period = props.limits.timestampPeriod;
  device->caps_.debug_utils = debug_utils;

  if (device->caps_.raytracing) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accel_props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &accel_props;
    vkGetPhysicalDeviceProperties2(device->physical_device_, &props2);
    device->caps_.accel_scratch_alignment =
        std::max<u32>(accel_props.minAccelerationStructureScratchOffsetAlignment, 1);
  }

  // Async compute queue: prefer a dedicated compute-only family (its own
  // hardware scheduling on most desktop GPUs, so compute genuinely overlaps
  // raster instead of round-robining one engine); fall back to a second
  // queue of the graphics family when no such family exists.
  u32 family_queue_count = 0;
  {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device_, &count, nullptr);
    base::Vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device_, &count, families.data());
    family_queue_count = families[device->graphics_family_].queueCount;
    // REC_ASYNC_DEDICATED=0 forces the same-family fallback (diagnostics;
    // also the only way to exercise that path on hardware with a dedicated
    // compute family).
    const char* dedicated_env = std::getenv("REC_ASYNC_DEDICATED");
    if (!dedicated_env || dedicated_env[0] != '0') {
      for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)) {
          device->compute_family_ = i;
          break;
        }
      }
    }
  }
  const bool dedicated_compute = device->compute_family_ != VK_QUEUE_FAMILY_IGNORED;
  const u32 queue_count =
      dedicated_compute ? 1 : std::min<u32>(family_queue_count, 2);
  device->caps_.async_compute = dedicated_compute || queue_count >= 2;
  if (!dedicated_compute) device->compute_family_ = device->graphics_family_;

  const f32 priorities[2] = {1.0f, 1.0f};
  VkDeviceQueueCreateInfo queue_infos[2] = {};
  queue_infos[0] = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_infos[0].queueFamilyIndex = device->graphics_family_;
  queue_infos[0].queueCount = queue_count;
  queue_infos[0].pQueuePriorities = priorities;
  queue_infos[1] = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_infos[1].queueFamilyIndex = device->compute_family_;
  queue_infos[1].queueCount = 1;
  queue_infos[1].pQueuePriorities = priorities;

  VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.pNext = &features;
  device_info.queueCreateInfoCount = dedicated_compute ? 2u : 1u;
  device_info.pQueueCreateInfos = queue_infos;
  device_info.enabledExtensionCount = static_cast<u32>(device_extensions.size());
  device_info.ppEnabledExtensionNames = device_extensions.data();

  if (vkCreateDevice(device->physical_device_, &device_info, nullptr, &device->device_) !=
      VK_SUCCESS) {
    REC_ERROR("vkCreateDevice failed");
    return nullptr;
  }
  volkLoadDevice(device->device_);
  vkGetDeviceQueue(device->device_, device->graphics_family_, 0, &device->graphics_queue_);
  if (device->caps_.async_compute) {
    if (dedicated_compute) {
      vkGetDeviceQueue(device->device_, device->compute_family_, 0, &device->compute_queue_);
      REC_INFO("async compute: dedicated queue family {}", device->compute_family_);
    } else {
      vkGetDeviceQueue(device->device_, device->graphics_family_, 1, &device->compute_queue_);
    }
  }

  // Pipeline cache, persisted to disk so shader compilation hitches only ever
  // happen once per driver/app version. Stale or corrupt blobs are rejected by
  // the driver, so loading is fire-and-forget.
  {
    const char* home = std::getenv("HOME");
    std::string dir = std::string(home ? home : ".") + "/.cache/recreation";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    device->pipeline_cache_path_ = dir + "/pipeline.cache";
    std::vector<char> blob;
    if (std::ifstream in{device->pipeline_cache_path_, std::ios::binary}) {
      blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo cache_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cache_info.initialDataSize = blob.size();
    cache_info.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(device->device_, &cache_info, nullptr,
                              &device->pipeline_cache_) != VK_SUCCESS) {
      cache_info.initialDataSize = 0;  // corrupt blob: start fresh
      cache_info.pInitialData = nullptr;
      vkCreatePipelineCache(device->device_, &cache_info, nullptr, &device->pipeline_cache_);
    }
    if (!blob.empty()) REC_INFO("pipeline cache: loaded {} KB", blob.size() >> 10);
  }

  if (!device->InitResources()) {
    return nullptr;
  }

  REC_INFO("gpu: {} ({}, {} MB, vk {}.{}, rt={} rayquery={} mesh={} vrs={})",
           device->caps_.adapter_name, device->caps_.integrated ? "integrated" : "discrete",
           device->caps_.device_local_bytes >> 20, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), device->caps_.raytracing,
           device->caps_.ray_query, device->caps_.mesh_shaders,
           device->caps_.fragment_shading_rate);
  return device;
}

std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, Window& window) {
  return VulkanDevice::Create(desc, window);
}

bool VulkanDevice::InitResources() {
  VmaVulkanFunctions functions{};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo info{};
  info.instance = instance_;
  info.physicalDevice = physical_device_;
  info.device = device_;
  info.vulkanApiVersion = VK_API_VERSION_1_3;
  info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  info.pVulkanFunctions = &functions;
  if (vmaCreateAllocator(&info, &allocator_) != VK_SUCCESS) {
    REC_ERROR("vma allocator creation failed");
    return false;
  }

  VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = graphics_family_;
  vkCreateCommandPool(device_, &pool_info, nullptr, &immediate_pool_);

  VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(device_, &fence_info, nullptr, &immediate_fence_);
  immediate_descriptor_pool_ = CreateTransientPool(device_);

  for (FrameRing& frame : frames_) {
    VkCommandPoolCreateInfo frame_pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    frame_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    frame_pool.queueFamilyIndex = graphics_family_;
    if (vkCreateCommandPool(device_, &frame_pool, nullptr, &frame.pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = frame.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &alloc, &frame.cmd);

    VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available);
    vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available_fg);
    VkFenceCreateInfo frame_fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    frame_fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &frame_fence, nullptr, &frame.in_flight);
    frame.descriptor_pool = CreateTransientPool(device_);
    frame.list = std::make_unique<VulkanCommandList>(*this, frame.cmd, frame.descriptor_pool);

    if (caps_.async_compute) {
      // Segment command buffers (graphics splits) share the frame's pool; the
      // async list allocates from a pool of the compute family (which is its
      // own pool even in the same-family fallback - pools are not thread- or
      // reset-granular, but the family must match the queue it submits to).
      VkCommandBuffer extra[FrameRing::kMaxSegments - 1] = {};
      alloc.commandBufferCount = FrameRing::kMaxSegments - 1;
      vkAllocateCommandBuffers(device_, &alloc, extra);
      for (u32 i = 0; i + 1 < FrameRing::kMaxSegments; ++i) {
        frame.seg_cmds[i] = extra[i];
        frame.seg_lists[i] =
            std::make_unique<VulkanCommandList>(*this, extra[i], frame.descriptor_pool);
      }
      VkCommandPoolCreateInfo async_pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      async_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      async_pool.queueFamilyIndex = compute_family_;
      if (vkCreateCommandPool(device_, &async_pool, nullptr, &frame.async_pool) != VK_SUCCESS) {
        return false;
      }
      VkCommandBufferAllocateInfo async_alloc{
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      async_alloc.commandPool = frame.async_pool;
      async_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      async_alloc.commandBufferCount = 1;
      vkAllocateCommandBuffers(device_, &async_alloc, &frame.async_cmd);
      frame.async_list = std::make_unique<VulkanCommandList>(
          *this, frame.async_cmd, frame.descriptor_pool,
          /*compute_only=*/compute_family_ != graphics_family_);
      vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.fork_sem);
      vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.async_sem);
    }
  }
  return true;
}

void VulkanDevice::ShutdownResources() {
  for (FrameRing& frame : frames_) {
    if (frame.descriptor_pool) vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
    if (frame.in_flight) vkDestroyFence(device_, frame.in_flight, nullptr);
    if (frame.image_available) vkDestroySemaphore(device_, frame.image_available, nullptr);
    if (frame.image_available_fg) vkDestroySemaphore(device_, frame.image_available_fg, nullptr);
    if (frame.fork_sem) vkDestroySemaphore(device_, frame.fork_sem, nullptr);
    if (frame.async_sem) vkDestroySemaphore(device_, frame.async_sem, nullptr);
    if (frame.pool) vkDestroyCommandPool(device_, frame.pool, nullptr);
    if (frame.async_pool) vkDestroyCommandPool(device_, frame.async_pool, nullptr);
    frame = {};
  }
  for (auto entry : sampler_cache_) vkDestroySampler(device_, entry.value, nullptr);
  for (auto entry : pipeline_layout_cache_) vkDestroyPipelineLayout(device_, entry.value, nullptr);
  for (auto entry : set_layout_cache_) vkDestroyDescriptorSetLayout(device_, entry.value, nullptr);
  sampler_cache_.clear();
  pipeline_layout_cache_.clear();
  set_layout_cache_.clear();
  if (immediate_descriptor_pool_)
    vkDestroyDescriptorPool(device_, immediate_descriptor_pool_, nullptr);
  if (immediate_fence_) vkDestroyFence(device_, immediate_fence_, nullptr);
  if (immediate_pool_) vkDestroyCommandPool(device_, immediate_pool_, nullptr);
  if (allocator_) vmaDestroyAllocator(allocator_);
  immediate_descriptor_pool_ = VK_NULL_HANDLE;
  immediate_fence_ = VK_NULL_HANDLE;
  immediate_pool_ = VK_NULL_HANDLE;
  allocator_ = nullptr;
}

VulkanDevice::~VulkanDevice() {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
    if (pipeline_cache_ != VK_NULL_HANDLE) {
      size_t size = 0;
      vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr);
      if (size > 0 && !pipeline_cache_path_.empty()) {
        std::vector<char> blob(size);
        if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, blob.data()) == VK_SUCCESS) {
          if (std::ofstream out{pipeline_cache_path_, std::ios::binary}) {
            out.write(blob.data(), static_cast<std::streamsize>(size));
          }
        }
      }
      vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    }
    ShutdownResources();
    vkDestroyDevice(device_, nullptr);
  }
  if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (debug_messenger_ != VK_NULL_HANDLE)
    vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

void VulkanDevice::WaitIdle() {
  if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
}

bool VulkanDevice::RecreateSurface(Window& window) {
  WaitIdle();
  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
  // Same physical device and queue family; the window is on the same display,
  // so present support carries over. Only the surface handle is rebound.
  if (!window.CreateVulkanSurface(instance_, &surface_)) {
    REC_ERROR("surface recreation failed");
    return false;
  }
  return true;
}

void VulkanDevice::DestroySurface() {
  if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
}

std::unique_ptr<Swapchain> VulkanDevice::CreateSwapchain(u32 width, u32 height, bool vsync,
                                                         bool hdr) {
  return VulkanSwapchain::Create(*this, width, height, vsync, hdr);
}

Device::MemoryBudget VulkanDevice::memory_budget() const {
  MemoryBudget result;
  if (!allocator_) return result;
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &props);
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(allocator_, budgets);
  for (u32 i = 0; i < props.memoryHeapCount; ++i) {
    if (!(props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
    result.used_bytes += budgets[i].usage;
    result.budget_bytes += budgets[i].budget;
    result.allocated_bytes += budgets[i].statistics.allocationBytes;
    result.allocation_count += budgets[i].statistics.allocationCount;
  }
  return result;
}

// --- resources ---

GpuBuffer VulkanDevice::CreateBuffer(u64 size, BufferUsageFlags usage, bool host_visible) {
  VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = ToVkBufferUsage(usage);
  // With a dedicated compute family any buffer may be read by the async
  // queue (bindless geometry, light/cluster buffers); CONCURRENT costs
  // nothing measurable for buffers and removes ownership-transfer churn.
  const u32 shared_families[2] = {graphics_family_, compute_family_};
  if (compute_family_ != graphics_family_) {
    buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_info.queueFamilyIndexCount = 2;
    buffer_info.pQueueFamilyIndices = shared_families;
  }

  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
  if (host_visible) {
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo result{};
  if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &buffer, &allocation, &result) !=
      VK_SUCCESS) {
    REC_ERROR("buffer allocation failed ({} bytes)", size);
    return {};
  }

  u64 address = 0;
  if (buffer_info.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo address_info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = buffer;
    address = vkGetBufferDeviceAddress(device_, &address_info);
  }

  auto* record = new BufferRecord{buffer, allocation};
  return {.handle = MakeHandle<BufferHandle>(record),
          .size = size,
          .mapped = result.pMappedData,
          .address = address};
}

GpuBuffer VulkanDevice::CreateBufferWithData(ByteSpan data, BufferUsageFlags usage) {
  GpuBuffer staging = CreateBuffer(data.size(), kBufferUsageTransferSrc, true);
  if (!staging.mapped) return {};
  std::memcpy(staging.mapped, data.data(), data.size());

  GpuBuffer buffer = CreateBuffer(data.size(), usage | kBufferUsageTransferDst, false);
  ImmediateSubmit([&](CommandList& cmd) {
    cmd.CopyBuffer(staging, 0, buffer, 0, data.size());
  });
  DestroyBuffer(staging);
  return buffer;
}

void VulkanDevice::DestroyBuffer(GpuBuffer& buffer) {
  if (BufferRecord* record = Rec(buffer.handle)) {
    if (record->buffer) vmaDestroyBuffer(allocator_, record->buffer, record->allocation);
    delete record;
  }
  buffer = {};
}


// Non-attachment images (sampled textures, storage volumes, probe atlases)
// may be read by the dedicated async compute family through bindless or
// direct binds; CONCURRENT skips ownership transfers. Attachments never
// cross and stay EXCLUSIVE so render targets keep framebuffer compression.
void VulkanDevice::ShareWithAsyncCompute(VkImageCreateInfo& info, TextureUsageFlags usage,
                                         const u32* families) {
  if (compute_family_ == graphics_family_) return;
  if (usage & (kTextureUsageColorTarget | kTextureUsageDepthTarget)) return;
  info.sharingMode = VK_SHARING_MODE_CONCURRENT;
  info.queueFamilyIndexCount = 2;
  info.pQueueFamilyIndices = families;
}

GpuImage VulkanDevice::CreateImage2D(Format format, Extent2D extent, TextureUsageFlags usage,
                                     u32 mip_levels) {
  VkFormat vk_format = ToVkFormat(format);
  VkImageCreateInfo image_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = vk_format;
  image_info.extent = {extent.width, extent.height, 1};
  image_info.mipLevels = mip_levels;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.usage = ToVkImageUsage(usage, format);

    const u32 shared_families[2] = {graphics_family_, compute_family_};
  ShareWithAsyncCompute(image_info, usage, shared_families);

VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
  alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // attachments

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkResult result =
      vmaCreateImage(allocator_, &image_info, &alloc_info, &image, &allocation, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("image allocation failed: result {} format {} {}x{} usage {:#x} mips {}",
              static_cast<int>(result), static_cast<int>(format), extent.width, extent.height,
              static_cast<u64>(usage), mip_levels);
    return {};
  }

  VkImageAspectFlags aspect = AspectOf(format);
  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = vk_format;
  view_info.subresourceRange = {aspect, 0, mip_levels, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device_, &view_info, nullptr, &view);

  auto* record = new TextureRecord{image, allocation, view, vk_format, aspect, mip_levels, 1};
  return {.handle = MakeHandle<TextureHandle>(record),
          .view = MakeView(view),
          .format = format,
          .extent = extent,
          .mip_levels = mip_levels};
}

GpuImage VulkanDevice::CreateImage3D(Format format, u32 width, u32 height, u32 depth,
                                     TextureUsageFlags usage) {
  VkFormat vk_format = ToVkFormat(format);
  VkImageCreateInfo image_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_3D;
  image_info.format = vk_format;
  image_info.extent = {width, height, depth};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.usage = ToVkImageUsage(usage, format);

    const u32 shared_families[2] = {graphics_family_, compute_family_};
  ShareWithAsyncCompute(image_info, usage, shared_families);

VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  if (vmaCreateImage(allocator_, &image_info, &alloc_info, &image, &allocation, nullptr) !=
      VK_SUCCESS) {
    REC_ERROR("3d image allocation failed: format {} {}x{}x{}", static_cast<int>(format), width,
              height, depth);
    return {};
  }

  VkImageAspectFlags aspect = AspectOf(format);
  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
  view_info.format = vk_format;
  view_info.subresourceRange = {aspect, 0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device_, &view_info, nullptr, &view);

  auto* record = new TextureRecord{image, allocation, view, vk_format, aspect, 1, 1};
  return {.handle = MakeHandle<TextureHandle>(record),
          .view = MakeView(view),
          .format = format,
          .extent = {width, height},
          .mip_levels = 1};
}

GpuImage VulkanDevice::CreateImageCube(Format format, u32 size, TextureUsageFlags usage,
                                       u32 mip_levels) {
  VkFormat vk_format = ToVkFormat(format);
  VkImageCreateInfo image_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = vk_format;
  image_info.extent = {size, size, 1};
  image_info.mipLevels = mip_levels;
  image_info.arrayLayers = 6;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.usage = ToVkImageUsage(usage, format);

    const u32 shared_families[2] = {graphics_family_, compute_family_};
  ShareWithAsyncCompute(image_info, usage, shared_families);

VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  if (vmaCreateImage(allocator_, &image_info, &alloc_info, &image, &allocation, nullptr) !=
      VK_SUCCESS) {
    REC_ERROR("cube image allocation failed");
    return {};
  }

  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  view_info.format = vk_format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 6};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device_, &view_info, nullptr, &view);

  auto* record = new TextureRecord{image,       allocation, view, vk_format,
                                   VK_IMAGE_ASPECT_COLOR_BIT, mip_levels, 6};
  return {.handle = MakeHandle<TextureHandle>(record),
          .view = MakeView(view),
          .format = format,
          .extent = {size, size},
          .mip_levels = mip_levels};
}

void VulkanDevice::DestroyImage(GpuImage& image) {
  if (TextureRecord* record = Rec(image.handle)) {
    if (record->view) vkDestroyImageView(device_, record->view, nullptr);
    if (record->image && record->allocation)
      vmaDestroyImage(allocator_, record->image, record->allocation);
    delete record;
  }
  image = {};
}

TextureView VulkanDevice::CreateMipView(const GpuImage& image, u32 mip) {
  const TextureRecord* record = Rec(image.handle);
  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = record->image;
  // Layered images (cube maps) get an array view over every layer so compute
  // passes can write all faces through one RWTexture2DArray binding.
  view_info.viewType =
      record->array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = record->format;
  view_info.subresourceRange = {record->aspect, mip, 1, 0, record->array_layers};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device_, &view_info, nullptr, &view);
  return MakeView(view);
}

TextureView VulkanDevice::CreateArrayView(const GpuImage& image) {
  const TextureRecord* record = Rec(image.handle);
  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = record->image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view_info.format = record->format;
  view_info.subresourceRange = {record->aspect, 0, record->mip_levels, 0, record->array_layers};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device_, &view_info, nullptr, &view);
  return MakeView(view);
}

void VulkanDevice::DestroyView(TextureView view) {
  if (view) vkDestroyImageView(device_, View(view), nullptr);
}

SamplerHandle VulkanDevice::GetSampler(const SamplerDesc& desc) {
  u64 key = HashBytes(&desc, sizeof(desc));
  if (VkSampler* cached = sampler_cache_.find(key)) {
    return SamplerHandle{reinterpret_cast<u64>(*cached)};
  }

  auto address = [](AddressMode mode) {
    switch (mode) {
      case AddressMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case AddressMode::kClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case AddressMode::kClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      case AddressMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  };

  VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = desc.mag_filter == Filter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = desc.min_filter == Filter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = desc.mip_filter == Filter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = address(desc.address_u);
  info.addressModeV = address(desc.address_v);
  info.addressModeW = address(desc.address_w);
  if (desc.max_anisotropy > 1.0f && caps_.max_anisotropy > 1.0f) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = std::min(desc.max_anisotropy, caps_.max_anisotropy);
  }
  info.minLod = desc.min_lod;
  info.maxLod = desc.max_lod;
  info.compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE;
  info.compareOp = ToVkCompare(desc.compare_op);
  switch (desc.border) {
    case BorderColor::kTransparentBlack:
      info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      break;
    case BorderColor::kOpaqueBlack: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
    case BorderColor::kOpaqueWhite: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
  }

  VkSampler sampler = VK_NULL_HANDLE;
  if (vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
    REC_ERROR("sampler creation failed");
    return {};
  }
  sampler_cache_.insert(key, sampler);
  return SamplerHandle{reinterpret_cast<u64>(sampler)};
}

// --- bindings ---

VkDescriptorSetLayout VulkanDevice::GetOrCreateSetLayout(const BindingLayoutDesc& desc) {
  u64 key = HashLayoutDesc(desc);
  if (VkDescriptorSetLayout* cached = set_layout_cache_.find(key)) return *cached;

  base::Vector<VkDescriptorSetLayoutBinding> bindings;
  base::Vector<VkDescriptorBindingFlags> flags;
  VkShaderStageFlags stages = ToVkStages(desc.stages);
  bool any_flags = false;
  for (const BindingSlot& slot : desc.slots) {
    bindings.push_back({.binding = slot.binding,
                        .descriptorType = ToVkDescriptorType(slot.type),
                        .descriptorCount = slot.count,
                        .stageFlags = stages});
    VkDescriptorBindingFlags flag = 0;
    if (slot.variable_count) {
      // Fixed-size, partially bound array: VARIABLE_DESCRIPTOR_COUNT would
      // force the binding to be the set's last, which the bindless layout
      // (sampler after the texture array) cannot satisfy.
      flag = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
             VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
      any_flags = true;
    }
    flags.push_back(flag);
  }

  VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  flags_info.bindingCount = static_cast<u32>(flags.size());
  flags_info.pBindingFlags = flags.data();

  VkDescriptorSetLayoutCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  if (any_flags || desc.update_after_bind) {
    info.pNext = &flags_info;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  }
  info.bindingCount = static_cast<u32>(bindings.size());
  info.pBindings = bindings.data();

  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout) != VK_SUCCESS) {
    REC_ERROR("descriptor set layout creation failed");
    return VK_NULL_HANDLE;
  }
  set_layout_cache_.insert(key, layout);
  return layout;
}

VkPipelineLayout VulkanDevice::GetOrCreatePipelineLayout(
    std::span<const VkDescriptorSetLayout> sets, VkShaderStageFlags push_stages, u32 push_size) {
  u64 key = HashBytes(sets.data(), sets.size() * sizeof(VkDescriptorSetLayout));
  key = HashBytes(&push_stages, sizeof(push_stages), key);
  key = HashBytes(&push_size, sizeof(push_size), key);
  if (VkPipelineLayout* cached = pipeline_layout_cache_.find(key)) return *cached;

  VkPushConstantRange push{push_stages, 0, push_size};
  VkPipelineLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = static_cast<u32>(sets.size());
  info.pSetLayouts = sets.data();
  if (push_size > 0) {
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
  }
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(device_, &info, nullptr, &layout) != VK_SUCCESS) {
    REC_ERROR("pipeline layout creation failed");
    return VK_NULL_HANDLE;
  }
  pipeline_layout_cache_.insert(key, layout);
  return layout;
}

BindingLayoutHandle VulkanDevice::CreateBindingLayout(const BindingLayoutDesc& desc) {
  VkDescriptorSetLayout layout = GetOrCreateSetLayout(desc);
  if (layout == VK_NULL_HANDLE) return {};
  auto* record = new BindingLayoutRecord{layout, desc};
  return MakeHandle<BindingLayoutHandle>(record);
}

void VulkanDevice::DestroyBindingLayout(BindingLayoutHandle layout) {
  delete Rec(layout);  // the VkDescriptorSetLayout itself is cached, freed at shutdown
}

BindingSetHandle VulkanDevice::CreateBindingSet(BindingLayoutHandle layout, u32 variable_count) {
  BindingLayoutRecord* layout_record = Rec(layout);

  // Dedicated pool sized exactly for this set; persistent sets are few.
  base::Vector<VkDescriptorPoolSize> sizes;
  for (const BindingSlot& slot : layout_record->desc.slots) {
    sizes.push_back({ToVkDescriptorType(slot.type), std::max(slot.count, 1u)});
  }
  VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  if (layout_record->desc.update_after_bind ||
      std::ranges::any_of(layout_record->desc.slots,
                          [](const BindingSlot& s) { return s.variable_count; })) {
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  }
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = static_cast<u32>(sizes.size());
  pool_info.pPoolSizes = sizes.data();
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    REC_ERROR("binding set pool creation failed");
    return {};
  }

  (void)variable_count;  // arrays are fixed-size, partially bound (see layout)
  VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &layout_record->layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device_, &alloc, &set) != VK_SUCCESS) {
    REC_ERROR("binding set allocation failed");
    vkDestroyDescriptorPool(device_, pool, nullptr);
    return {};
  }
  auto* record = new BindingSetRecord{set, pool, layout_record};
  return MakeHandle<BindingSetHandle>(record);
}

void VulkanDevice::DestroyBindingSet(BindingSetHandle set) {
  if (BindingSetRecord* record = Rec(set)) {
    if (record->pool) vkDestroyDescriptorPool(device_, record->pool, nullptr);
    delete record;
  }
}

void VulkanDevice::WriteDescriptors(VkDescriptorSet set, std::span<const BindingItem> items) {
  base::Vector<VkWriteDescriptorSet> writes(items.size());
  base::Vector<VkDescriptorImageInfo> images(items.size());
  base::Vector<VkDescriptorBufferInfo> buffers(items.size());
  base::Vector<VkWriteDescriptorSetAccelerationStructureKHR> accels(items.size());
  base::Vector<VkAccelerationStructureKHR> accel_handles(items.size());

  for (size_t i = 0; i < items.size(); ++i) {
    const BindingItem& item = items[i];
    VkWriteDescriptorSet& write = writes[i];
    write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = item.slot;
    write.dstArrayElement = item.array_index;
    write.descriptorCount = 1;
    write.descriptorType = ToVkDescriptorType(item.type);
    switch (item.type) {
      case BindingType::kStorageImage:
        images[i] = {.imageView = View(item.view), .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        write.pImageInfo = &images[i];
        break;
      case BindingType::kSampledImage:
        images[i] = {.imageView = View(item.view),
                     .imageLayout = item.general_layout
                                        ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        write.pImageInfo = &images[i];
        break;
      case BindingType::kCombinedTextureSampler:
        images[i] = {.sampler = SamplerOf(item.sampler),
                     .imageView = View(item.view),
                     .imageLayout = item.general_layout
                                        ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        write.pImageInfo = &images[i];
        break;
      case BindingType::kSampler:
        images[i] = {.sampler = SamplerOf(item.sampler)};
        write.pImageInfo = &images[i];
        break;
      case BindingType::kUniformBuffer:
      case BindingType::kStorageBuffer:
        buffers[i] = {.buffer = Rec(item.buffer)->buffer,
                      .offset = item.buffer_offset,
                      .range = item.buffer_range ? item.buffer_range : VK_WHOLE_SIZE};
        write.pBufferInfo = &buffers[i];
        break;
      case BindingType::kAccelStruct:
        accel_handles[i] = Rec(item.accel)->accel;
        accels[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        accels[i].accelerationStructureCount = 1;
        accels[i].pAccelerationStructures = &accel_handles[i];
        write.pNext = &accels[i];
        break;
    }
  }
  vkUpdateDescriptorSets(device_, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanDevice::UpdateBindingSet(BindingSetHandle set, std::span<const BindingItem> items) {
  WriteDescriptors(Rec(set)->set, items);
}

// --- pipelines ---

namespace {

// Resolves a pipeline desc's set interface to Vulkan layouts. `stages` is the
// fallback when a PipelineBindings entry leaves stages at 0.
bool ResolveSetLayouts(VulkanDevice& device, const base::Vector<PipelineBindings>& sets,
                       ShaderStageFlags stages,
                       base::Vector<VkDescriptorSetLayout>* out) {
  for (const PipelineBindings& set : sets) {
    if (set.shared) {
      out->push_back(Rec(set.shared)->layout);
      continue;
    }
    BindingLayoutDesc desc;
    desc.stages = set.stages ? set.stages : stages;
    desc.slots = set.slots;
    VkDescriptorSetLayout layout = device.GetOrCreateSetLayout(desc);
    if (layout == VK_NULL_HANDLE) return false;
    out->push_back(layout);
  }
  return true;
}

}  // namespace

PipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
  base::Vector<VkDescriptorSetLayout> set_layouts;
  if (!ResolveSetLayouts(*this, desc.sets, kShaderStageCompute, &set_layouts)) return {};

  VkPipelineLayout layout = GetOrCreatePipelineLayout(
      {set_layouts.data(), set_layouts.size()}, VK_SHADER_STAGE_COMPUTE_BIT,
      desc.push_constant_size);
  if (layout == VK_NULL_HANDLE) return {};

  VkShaderModule module = CreateModule(device_, desc.shader);
  if (module == VK_NULL_HANDLE) {
    REC_ERROR("compute shader module creation failed ({})",
              desc.debug_name ? desc.debug_name : "?");
    return {};
  }

  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = vkCreateComputePipelines(device_, pipeline_cache_, 1, &info, nullptr,
                                             &pipeline);
  vkDestroyShaderModule(device_, module, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("compute pipeline creation failed ({})", desc.debug_name ? desc.debug_name : "?");
    return {};
  }

  auto* record = new PipelineRecord{pipeline, layout, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    VK_SHADER_STAGE_COMPUTE_BIT, desc.push_constant_size,
                                    std::move(set_layouts)};
  return MakeHandle<PipelineHandle>(record);
}

PipelineHandle VulkanDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
  bool mesh_path = desc.mesh.valid();
  ShaderStageFlags all_stages =
      mesh_path ? (kShaderStageTask | kShaderStageMesh | kShaderStageFragment)
                : (kShaderStageVertex | kShaderStageFragment);

  base::Vector<VkDescriptorSetLayout> set_layouts;
  if (!ResolveSetLayouts(*this, desc.sets, all_stages, &set_layouts)) return {};

  VkShaderStageFlags push_stages = ToVkStages(all_stages);
  VkPipelineLayout layout = GetOrCreatePipelineLayout(
      {set_layouts.data(), set_layouts.size()}, push_stages, desc.push_constant_size);
  if (layout == VK_NULL_HANDLE) return {};

  VkPipelineShaderStageCreateInfo stages[3];
  u32 stage_count = 0;
  base::Vector<VkShaderModule> modules;
  auto add_stage = [&](const ShaderBlob& blob, VkShaderStageFlagBits stage) {
    if (!blob.valid()) return true;
    VkShaderModule module = CreateModule(device_, blob);
    if (module == VK_NULL_HANDLE) return false;
    modules.push_back(module);
    stages[stage_count] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[stage_count].stage = stage;
    stages[stage_count].module = module;
    stages[stage_count].pName = "main";
    ++stage_count;
    return true;
  };
  bool ok = mesh_path
                ? add_stage(desc.task, VK_SHADER_STAGE_TASK_BIT_EXT) &&
                      add_stage(desc.mesh, VK_SHADER_STAGE_MESH_BIT_EXT) &&
                      add_stage(desc.fragment, VK_SHADER_STAGE_FRAGMENT_BIT)
                : add_stage(desc.vertex, VK_SHADER_STAGE_VERTEX_BIT) &&
                      add_stage(desc.fragment, VK_SHADER_STAGE_FRAGMENT_BIT);
  auto destroy_modules = [&] {
    for (VkShaderModule module : modules) vkDestroyShaderModule(device_, module, nullptr);
  };
  if (!ok) {
    destroy_modules();
    REC_ERROR("graphics shader module creation failed ({})",
              desc.debug_name ? desc.debug_name : "?");
    return {};
  }

  base::Vector<VkVertexInputBindingDescription> vertex_bindings;
  base::Vector<VkVertexInputAttributeDescription> vertex_attributes;
  for (u32 binding = 0; binding < desc.vertex_buffers.size(); ++binding) {
    const VertexBufferLayout& buffer = desc.vertex_buffers[binding];
    vertex_bindings.push_back({.binding = binding,
                               .stride = buffer.stride,
                               .inputRate = buffer.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                : VK_VERTEX_INPUT_RATE_VERTEX});
    for (const VertexAttribute& attribute : buffer.attributes) {
      vertex_attributes.push_back({.location = attribute.location,
                                   .binding = binding,
                                   .format = ToVkFormat(attribute.format),
                                   .offset = attribute.offset});
    }
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = static_cast<u32>(vertex_bindings.size());
  vertex_input.pVertexBindingDescriptions = vertex_bindings.data();
  vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes.size());
  vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();

  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  switch (desc.topology) {
    case PrimitiveTopology::kTriangleList:
      input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case PrimitiveTopology::kTriangleStrip:
      input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PrimitiveTopology::kLineList:
      input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case PrimitiveTopology::kPointList:
      input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      break;
  }

  VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode =
      desc.raster.polygon == PolygonMode::kLine ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = desc.raster.cull == CullMode::kNone   ? VK_CULL_MODE_NONE
                    : desc.raster.cull == CullMode::kBack ? VK_CULL_MODE_BACK_BIT
                                                          : VK_CULL_MODE_FRONT_BIT;
  raster.frontFace = desc.raster.front == FrontFace::kClockwise
                         ? VK_FRONT_FACE_CLOCKWISE
                         : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  if (desc.depth.bias_constant != 0.0f || desc.depth.bias_slope != 0.0f) {
    raster.depthBiasEnable = VK_TRUE;
    raster.depthBiasConstantFactor = desc.depth.bias_constant;
    raster.depthBiasSlopeFactor = desc.depth.bias_slope;
  }

  VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = desc.depth.test ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = ToVkCompare(desc.depth.compare);

  base::Vector<VkPipelineColorBlendAttachmentState> blend_states(desc.color_formats.size());
  for (size_t i = 0; i < desc.color_formats.size(); ++i) {
    BlendMode mode = i < desc.blend.size() ? desc.blend[i] : BlendMode::kOpaque;
    BlendConfig config = BlendConfigOf(mode);
    VkPipelineColorBlendAttachmentState& state = blend_states[i];
    state = {};
    state.blendEnable = config.enable ? VK_TRUE : VK_FALSE;
    state.srcColorBlendFactor = config.src_color;
    state.dstColorBlendFactor = config.dst_color;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = config.src_alpha;
    state.dstAlphaBlendFactor = config.dst_alpha;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  }
  VkPipelineColorBlendStateCreateInfo blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = static_cast<u32>(blend_states.size());
  blend.pAttachments = blend_states.data();

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  base::Vector<VkFormat> color_formats(desc.color_formats.size());
  for (size_t i = 0; i < desc.color_formats.size(); ++i) {
    color_formats[i] = ToVkFormat(desc.color_formats[i]);
  }
  VkPipelineRenderingCreateInfo rendering{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = static_cast<u32>(color_formats.size());
  rendering.pColorAttachmentFormats = color_formats.data();
  rendering.depthAttachmentFormat = ToVkFormat(desc.depth.format);

  // When attachment VRS is available every graphics pipeline honors an
  // attached rate image (combiner REPLACE); passes without one shade at 1x1,
  // so a single pipeline set serves both VRS-on and VRS-off render passes.
  VkPipelineFragmentShadingRateStateCreateInfoKHR fsr_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
  fsr_state.fragmentSize = {1, 1};
  fsr_state.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
  fsr_state.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
  if (caps_.fragment_shading_rate) rendering.pNext = &fsr_state;

  VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  if (caps_.fragment_shading_rate) {
    info.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
  }
  info.stageCount = stage_count;
  info.pStages = stages;
  if (!mesh_path) {
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
  }
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr,
                                              &pipeline);
  destroy_modules();
  if (result != VK_SUCCESS) {
    REC_ERROR("graphics pipeline creation failed ({})", desc.debug_name ? desc.debug_name : "?");
    return {};
  }

  auto* record = new PipelineRecord{pipeline, layout, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    push_stages, desc.push_constant_size, std::move(set_layouts)};
  return MakeHandle<PipelineHandle>(record);
}

void VulkanDevice::DestroyPipeline(PipelineHandle pipeline) {
  if (PipelineRecord* record = Rec(pipeline)) {
    if (record->pipeline) vkDestroyPipeline(device_, record->pipeline, nullptr);
    // Layouts are cached and shared; freed at shutdown.
    delete record;
  }
}

// --- acceleration structures ---

AccelSizes VulkanDevice::GetBlasSizes(const BlasBuildDesc& desc) {
  base::Vector<VkAccelerationStructureGeometryKHR> geometries;
  base::Vector<u32> primitive_counts;
  for (const AccelTriangles& t : desc.geometries) {
    VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    auto& tri = geometry.geometry.triangles;
    tri = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.vertexFormat = ToVkFormat(t.vertex_format);
    tri.vertexData.deviceAddress = t.vertex_address;
    tri.vertexStride = t.vertex_stride;
    tri.maxVertex = t.vertex_count == 0 ? 0 : t.vertex_count - 1;
    tri.indexType =
        t.index_type == IndexType::kUint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = t.index_address;
    geometries.push_back(geometry);
    primitive_counts.push_back(t.index_count / 3);
  }

  VkAccelerationStructureBuildGeometryInfoKHR info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  info.flags = desc.fast_trace ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                               : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  info.geometryCount = static_cast<u32>(geometries.size());
  info.pGeometries = geometries.data();

  VkAccelerationStructureBuildSizesInfoKHR sizes{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_,
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                          primitive_counts.data(), &sizes);
  return {sizes.accelerationStructureSize, sizes.buildScratchSize};
}

AccelSizes VulkanDevice::GetTlasSizes(u32 instance_count) {
  VkAccelerationStructureGeometryKHR geometry{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};

  VkAccelerationStructureBuildGeometryInfoKHR info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  info.geometryCount = 1;
  info.pGeometries = &geometry;

  VkAccelerationStructureBuildSizesInfoKHR sizes{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_,
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                          &instance_count, &sizes);
  return {sizes.accelerationStructureSize, sizes.buildScratchSize};
}

AccelStructHandle VulkanDevice::CreateAccelStruct(AccelStructType type, u64 size) {
  VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const u32 shared_families[2] = {graphics_family_, compute_family_};
  if (compute_family_ != graphics_family_) {
    // Built on graphics, ray-queried by DDGI on the async compute queue.
    buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_info.queueFamilyIndexCount = 2;
    buffer_info.pQueueFamilyIndices = shared_families;
  }
  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

  BufferRecord backing;
  if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &backing.buffer,
                      &backing.allocation, nullptr) != VK_SUCCESS) {
    REC_ERROR("acceleration structure backing allocation failed ({} bytes)", size);
    return {};
  }

  VkAccelerationStructureCreateInfoKHR info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  info.buffer = backing.buffer;
  info.size = size;
  info.type = type == AccelStructType::kBlas ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
                                             : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
  if (vkCreateAccelerationStructureKHR(device_, &info, nullptr, &accel) != VK_SUCCESS) {
    vmaDestroyBuffer(allocator_, backing.buffer, backing.allocation);
    REC_ERROR("acceleration structure creation failed");
    return {};
  }

  VkAccelerationStructureDeviceAddressInfoKHR address_info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  address_info.accelerationStructure = accel;
  u64 address = vkGetAccelerationStructureDeviceAddressKHR(device_, &address_info);

  auto* record = new AccelStructRecord{accel, backing, address};
  return MakeHandle<AccelStructHandle>(record);
}

void VulkanDevice::DestroyAccelStruct(AccelStructHandle handle) {
  if (AccelStructRecord* record = Rec(handle)) {
    if (record->accel) vkDestroyAccelerationStructureKHR(device_, record->accel, nullptr);
    if (record->backing.buffer)
      vmaDestroyBuffer(allocator_, record->backing.buffer, record->backing.allocation);
    delete record;
  }
}

u64 VulkanDevice::accel_address(AccelStructHandle handle) { return Rec(handle)->address; }

// --- profiling ---

TimestampPoolHandle VulkanDevice::CreateTimestampPool(u32 count) {
  VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = count;
  VkQueryPool pool = VK_NULL_HANDLE;
  if (vkCreateQueryPool(device_, &info, nullptr, &pool) != VK_SUCCESS) return {};
  auto* record = new TimestampPoolRecord{pool, count};
  return MakeHandle<TimestampPoolHandle>(record);
}

void VulkanDevice::DestroyTimestampPool(TimestampPoolHandle pool) {
  if (TimestampPoolRecord* record = Rec(pool)) {
    if (record->pool) vkDestroyQueryPool(device_, record->pool, nullptr);
    delete record;
  }
}

bool VulkanDevice::GetTimestamps(TimestampPoolHandle pool, u32 first, u32 count, u64* out) {
  return vkGetQueryPoolResults(device_, Rec(pool)->pool, first, count, count * sizeof(u64), out,
                               sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
}

// --- recording & submission ---

void VulkanDevice::ImmediateSubmit(const std::function<void(CommandList&)>& record) {
  VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = immediate_pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device_, &alloc, &cmd);

  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  VulkanCommandList list(*this, cmd, immediate_descriptor_pool_);
  record(list);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(graphics_queue_, 1, &submit, immediate_fence_);
  vkWaitForFences(device_, 1, &immediate_fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &immediate_fence_);
  vkFreeCommandBuffers(device_, immediate_pool_, 1, &cmd);
  vkResetDescriptorPool(device_, immediate_descriptor_pool_, 0);
}

CommandList* VulkanDevice::BeginFrame(u32 slot) {
  FrameRing& frame = frames_[slot];
  vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
  vkResetCommandPool(device_, frame.pool, 0);
  if (frame.async_pool) vkResetCommandPool(device_, frame.async_pool, 0);
  vkResetDescriptorPool(device_, frame.descriptor_pool, 0);
  current_slot_ = slot;
  frame.active_segment = 0;
  frame.fork_signaled = false;
  frame.async_submitted = false;

  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.cmd, &begin);
  return frame.list.get();
}

CommandList* VulkanDevice::SplitFrame(CommandList* cmd, bool signal_fork) {
  FrameRing& frame = frames_[current_slot_];
  if (!caps_.async_compute || frame.active_segment + 1 >= FrameRing::kMaxSegments) return cmd;

  VkCommandBuffer current =
      frame.active_segment == 0 ? frame.cmd : frame.seg_cmds[frame.active_segment - 1];
  vkEndCommandBuffer(current);

  VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cmd_info.commandBuffer = current;
  VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = frame.fork_sem;
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd_info;
  if (signal_fork) {
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    frame.fork_signaled = true;
  }
  vkQueueSubmit2(graphics_queue_, 1, &submit, VK_NULL_HANDLE);

  VkCommandBuffer next = frame.seg_cmds[frame.active_segment];
  ++frame.active_segment;
  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(next, &begin);
  return frame.seg_lists[frame.active_segment - 1].get();
}

CommandList* VulkanDevice::BeginAsync() {
  FrameRing& frame = frames_[current_slot_];
  if (!caps_.async_compute) return nullptr;
  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.async_cmd, &begin);
  return frame.async_list.get();
}

void VulkanDevice::SubmitAsync(CommandList*) {
  FrameRing& frame = frames_[current_slot_];
  vkEndCommandBuffer(frame.async_cmd);

  VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cmd_info.commandBuffer = frame.async_cmd;
  VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = frame.fork_sem;
  wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = frame.async_sem;
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  if (frame.fork_signaled) {
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    frame.fork_signaled = false;  // consumed
  }
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd_info;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  vkQueueSubmit2(compute_queue_, 1, &submit, VK_NULL_HANDLE);
  frame.async_submitted = true;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REC_INFO("async compute: first submission (family {})", compute_family_);
  }
}

PresentResult VulkanDevice::SubmitFrame(CommandList* cmd, Swapchain& swapchain, u32 image_index) {
  auto& vk_swapchain = static_cast<VulkanSwapchain&>(swapchain);
  FrameRing* frame = nullptr;
  for (FrameRing& candidate : frames_) {
    if (candidate.list.get() == cmd) frame = &candidate;
    for (const auto& seg : candidate.seg_lists) {
      if (seg && seg.get() == cmd) frame = &candidate;
    }
  }
  if (!frame) return PresentResult::kFailed;

  VkCommandBuffer active =
      frame->active_segment == 0 ? frame->cmd : frame->seg_cmds[frame->active_segment - 1];
  vkEndCommandBuffer(active);
  // Only reset once the frame is guaranteed to submit, so an aborted record
  // cannot deadlock the next BeginFrame's wait.
  vkResetFences(device_, 1, &frame->in_flight);

  VkSemaphoreSubmitInfo waits[2];
  waits[0] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  waits[0].semaphore = frame->image_available;
  waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  u32 wait_count = 1;
  if (frame->async_submitted) {
    // Join: the final segment consumes the async queue's results.
    waits[1] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waits[1].semaphore = frame->async_sem;
    waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    wait_count = 2;
    frame->async_submitted = false;
  }
  VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = vk_swapchain.render_finished(image_index);
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cmd_info.commandBuffer = active;

  VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = wait_count;
  submit.pWaitSemaphoreInfos = waits;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd_info;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  vkQueueSubmit2(graphics_queue_, 1, &submit, frame->in_flight);

  VkSemaphore present_wait = vk_swapchain.render_finished(image_index);
  VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &present_wait;
  present.swapchainCount = 1;
  VkSwapchainKHR handle = vk_swapchain.swapchain();
  present.pSwapchains = &handle;
  present.pImageIndices = &image_index;
  VkResult presented = vkQueuePresentKHR(graphics_queue_, &present);
  return TranslatePresent(presented, swapchain);
}

PresentResult VulkanDevice::SubmitFrameGen(CommandList* cmd, Swapchain& swapchain,
                                           u32 interp_index, u32 real_index) {
  auto& vk_swapchain = static_cast<VulkanSwapchain&>(swapchain);
  FrameRing* frame = nullptr;
  for (FrameRing& candidate : frames_) {
    if (candidate.list.get() == cmd) frame = &candidate;
    for (const auto& seg : candidate.seg_lists) {
      if (seg && seg.get() == cmd) frame = &candidate;
    }
  }
  if (!frame) return PresentResult::kFailed;

  VkCommandBuffer active =
      frame->active_segment == 0 ? frame->cmd : frame->seg_cmds[frame->active_segment - 1];
  vkEndCommandBuffer(active);
  vkResetFences(device_, 1, &frame->in_flight);

  // Wait both acquires; the interpolated image is written by a transfer, the
  // real one by the color-attachment output, so gate at those stages.
  VkSemaphoreSubmitInfo waits[3];
  waits[0] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  waits[0].semaphore = frame->image_available;
  waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  waits[1] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  waits[1].semaphore = frame->image_available_fg;
  waits[1].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  u32 wait_count = 2;
  if (frame->async_submitted) {
    waits[2] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waits[2].semaphore = frame->async_sem;
    waits[2].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    wait_count = 3;
    frame->async_submitted = false;
  }
  // Both presents get their own render-finished semaphore, signaled by this
  // one submission.
  VkSemaphoreSubmitInfo signals[2];
  signals[0] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signals[0].semaphore = vk_swapchain.render_finished(interp_index);
  signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  signals[1] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signals[1].semaphore = vk_swapchain.render_finished(real_index);
  signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

  VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cmd_info.commandBuffer = active;
  VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = wait_count;
  submit.pWaitSemaphoreInfos = waits;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd_info;
  submit.signalSemaphoreInfoCount = 2;
  submit.pSignalSemaphoreInfos = signals;
  vkQueueSubmit2(graphics_queue_, 1, &submit, frame->in_flight);

  // Interpolated (the older midpoint) first, then the fresh frame; FIFO paces
  // them one vblank apart.
  VkSwapchainKHR handle = vk_swapchain.swapchain();
  VkResult presented = VK_SUCCESS;
  const u32 indices[2] = {interp_index, real_index};
  for (u32 i = 0; i < 2; ++i) {
    VkSemaphore wait_sem = vk_swapchain.render_finished(indices[i]);
    VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &wait_sem;
    present.swapchainCount = 1;
    present.pSwapchains = &handle;
    present.pImageIndices = &indices[i];
    VkResult r = vkQueuePresentKHR(graphics_queue_, &present);
    if (r != VK_SUCCESS) presented = r;
  }
  return TranslatePresent(presented, swapchain);
}

PresentResult VulkanDevice::TranslatePresent(VkResult presented, Swapchain& swapchain) {

  if (presented == VK_ERROR_OUT_OF_DATE_KHR) return PresentResult::kOutOfDate;
  if (presented == VK_SUBOPTIMAL_KHR) {
    // Android reports SUBOPTIMAL every frame because preTransform (identity)
    // differs from the panel's rotation, which is stable and handled by the
    // display engine; recreating on that alone would rebuild every frame. Only
    // recreate when the surface extent actually changed (a real resize).
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    if (caps.currentExtent.width != 0xffffffffu &&
        (caps.currentExtent.width != swapchain.extent().width ||
         caps.currentExtent.height != swapchain.extent().height)) {
      return PresentResult::kOutOfDate;
    }
    return PresentResult::kOk;
  }
  return presented == VK_SUCCESS ? PresentResult::kOk : PresentResult::kFailed;
}

}  // namespace rec::render::vk

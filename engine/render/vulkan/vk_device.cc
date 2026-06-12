#include <algorithm>
#include <cstring>

#include <base/containers/vector.h>

#include "core/log.h"
#include "render/rhi/device.h"

namespace rec::render {
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

}  // namespace

std::unique_ptr<Device> Device::Create(const DeviceDesc& desc, Window& window) {
  auto device = std::unique_ptr<Device>(new Device());

  auto surface_extensions = window.vulkan_instance_extensions();
  if (surface_extensions.empty()) {
    REC_WARN("window cannot present, renderer disabled");
    return device;
  }
  if (volkInitialize() != VK_SUCCESS) {
    REC_WARN("no vulkan loader on this system, renderer disabled");
    return device;
  }
  if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
    REC_ERROR("vulkan 1.3 required, loader reports {}.{}",
              VK_API_VERSION_MAJOR(volkGetInstanceVersion()),
              VK_API_VERSION_MINOR(volkGetInstanceVersion()));
    return device;
  }

  base::Vector<const char*> instance_extensions;
  instance_extensions.assign(surface_extensions.begin(), surface_extensions.end());
  base::Vector<const char*> layers;
  bool validation = desc.enable_validation && HasValidationLayer();
  if (validation) {
    layers.push_back(kValidationLayer);
    instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  } else if (desc.enable_validation) {
    REC_WARN("validation requested but layer not installed");
  }

  VkApplicationInfo app_info{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "recreation";
  app_info.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app_info;
  instance_info.enabledLayerCount = static_cast<u32>(layers.size());
  instance_info.ppEnabledLayerNames = layers.data();
  instance_info.enabledExtensionCount = static_cast<u32>(instance_extensions.size());
  instance_info.ppEnabledExtensionNames = instance_extensions.data();

  if (vkCreateInstance(&instance_info, nullptr, &device->instance_) != VK_SUCCESS) {
    REC_ERROR("vkCreateInstance failed");
    return device;
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
    return device;
  }

  device->physical_device_ = PickPhysicalDevice(device->instance_, device->surface_);
  if (device->physical_device_ == VK_NULL_HANDLE) {
    REC_ERROR("no vulkan 1.3 capable gpu found");
    return device;
  }

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(device->physical_device_, &props);
  device->caps_.adapter_name = props.deviceName;
  device->caps_.api_version = props.apiVersion;
  device->graphics_family_ =
      static_cast<u32>(FindGraphicsFamily(device->physical_device_, device->surface_));

  auto available = DeviceExtensions(device->physical_device_);
  base::Vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

  bool want_raytracing = desc.request_raytracing &&
                         HasExtension(available, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                         HasExtension(available, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                         HasExtension(available, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  bool want_ray_query =
      want_raytracing && HasExtension(available, VK_KHR_RAY_QUERY_EXTENSION_NAME);
  bool want_mesh_shaders = HasExtension(available, VK_EXT_MESH_SHADER_EXTENSION_NAME);
  bool want_shading_rate = HasExtension(available, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);

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

  vkGetPhysicalDeviceFeatures2(device->physical_device_, &features);

  if (!f13.dynamicRendering || !f13.synchronization2 || !f12.bufferDeviceAddress ||
      !f12.timelineSemaphore || !f12.descriptorIndexing) {
    REC_ERROR("{} lacks the required vulkan 1.3 baseline", device->caps_.adapter_name);
    return device;
  }
  // Costs measurable bandwidth and the engine does its own bounds discipline.
  features.features.robustBufferAccess = VK_FALSE;

  device->caps_.raytracing = accel.accelerationStructure && rt_pipeline.rayTracingPipeline;
  device->caps_.ray_query = ray_query.rayQuery;
  device->caps_.mesh_shaders = mesh.meshShader;
  device->caps_.fragment_shading_rate = shading_rate.pipelineFragmentShadingRate;
  device->caps_.fill_mode_non_solid = features.features.fillModeNonSolid;
  if (features.features.samplerAnisotropy) {
    device->caps_.max_anisotropy = props.limits.maxSamplerAnisotropy;
  }

  f32 priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = device->graphics_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.pNext = &features;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = static_cast<u32>(device_extensions.size());
  device_info.ppEnabledExtensionNames = device_extensions.data();

  if (vkCreateDevice(device->physical_device_, &device_info, nullptr, &device->device_) !=
      VK_SUCCESS) {
    REC_ERROR("vkCreateDevice failed");
    return device;
  }
  volkLoadDevice(device->device_);
  vkGetDeviceQueue(device->device_, device->graphics_family_, 0, &device->graphics_queue_);

  if (!device->InitResources()) {
    vkDestroyDevice(device->device_, nullptr);
    device->device_ = VK_NULL_HANDLE;
    return device;
  }

  REC_INFO("gpu: {} (vk {}.{}, rt={} rayquery={} mesh={} vrs={})", device->caps_.adapter_name,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           device->caps_.raytracing, device->caps_.ray_query, device->caps_.mesh_shaders,
           device->caps_.fragment_shading_rate);
  return device;
}

Device::~Device() {
  if (device_ != VK_NULL_HANDLE) ShutdownResources();
  if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
  if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (debug_messenger_ != VK_NULL_HANDLE)
    vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

void Device::WaitIdle() {
  if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
}

}  // namespace rec::render

#include "render/rhi/device.h"

#include "core/log.h"

#if defined(RECREATION_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace rec::render {

#if defined(RECREATION_HAS_VULKAN)

struct Device::VulkanState {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  u32 graphics_family = 0;
};

namespace {

VkInstance CreateInstance(bool enable_validation) {
  VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "recreation";
  app_info.apiVersion = VK_API_VERSION_1_3;

  const char* validation_layer = "VK_LAYER_KHRONOS_validation";
  VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  create_info.pApplicationInfo = &app_info;
  if (enable_validation) {
    create_info.enabledLayerCount = 1;
    create_info.ppEnabledLayerNames = &validation_layer;
  }

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
    REC_ERROR("vkCreateInstance failed");
  }
  return instance;
}

}  // namespace

std::unique_ptr<Device> Device::Create(const DeviceDesc& desc, const NativeWindowHandles& window) {
  auto device = std::unique_ptr<Device>(new Device());
  device->vk_ = std::make_unique<VulkanState>();
  device->vk_->instance = CreateInstance(desc.enable_validation);
  if (device->vk_->instance == VK_NULL_HANDLE) return device;

  u32 count = 0;
  vkEnumeratePhysicalDevices(device->vk_->instance, &count, nullptr);
  if (count == 0) {
    REC_ERROR("no vulkan capable gpu found");
    return device;
  }
  std::vector<VkPhysicalDevice> physical_devices(count);
  vkEnumeratePhysicalDevices(device->vk_->instance, &count, physical_devices.data());
  device->vk_->physical_device = physical_devices[0];

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(device->vk_->physical_device, &props);
  device->caps_.adapter_name = props.deviceName;

  // TODO: queue selection, logical device, surface and swapchain, and the
  // raytracing/mesh shader extension queries feeding caps_.
  device->is_stub_ = false;
  REC_INFO("vulkan device: {}", device->caps_.adapter_name);
  return device;
}

Device::~Device() {
  if (vk_ && vk_->device != VK_NULL_HANDLE) vkDestroyDevice(vk_->device, nullptr);
  if (vk_ && vk_->instance != VK_NULL_HANDLE) vkDestroyInstance(vk_->instance, nullptr);
}

void Device::WaitIdle() {
  if (vk_ && vk_->device != VK_NULL_HANDLE) vkDeviceWaitIdle(vk_->device);
}

#else

struct Device::VulkanState {};

std::unique_ptr<Device> Device::Create(const DeviceDesc&, const NativeWindowHandles&) {
  REC_WARN("built without vulkan, renderer is a stub");
  return std::unique_ptr<Device>(new Device());
}

Device::~Device() = default;

void Device::WaitIdle() {}

#endif

}  // namespace rec::render

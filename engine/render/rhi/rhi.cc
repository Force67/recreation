#include "core/log.h"
#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"

namespace rec::render {

// Backend factories, defined in their backend directories. Availability is a
// build-time property (RECREATION_RHI_*); returning null means "not available
// on this machine", and Create falls through to the next candidate.
namespace vk {
std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, Window& window);
}
namespace d3d12 {
std::unique_ptr<Device> CreateD3D12Device(const DeviceDesc& desc, Window& window);
}
namespace null {
std::unique_ptr<Device> CreateNullDevice();
}

const char* BackendName(Backend backend) {
  switch (backend) {
    case Backend::kAuto: return "auto";
    case Backend::kVulkan: return "vulkan";
    case Backend::kD3D12: return "d3d12";
    case Backend::kNull: return "null";
  }
  return "?";
}

std::unique_ptr<Device> Device::Create(const DeviceDesc& desc, Window& window) {
  std::unique_ptr<Device> device;
  switch (desc.backend) {
    case Backend::kVulkan:
#if defined(RECREATION_RHI_VULKAN)
      device = vk::CreateVulkanDevice(desc, window);
#endif
      break;
    case Backend::kD3D12:
#if defined(RECREATION_RHI_D3D12)
      device = d3d12::CreateD3D12Device(desc, window);
#endif
      break;
    case Backend::kNull:
      break;
    case Backend::kAuto:
      // Platform-native first, then vulkan. The null fallback below keeps the
      // engine alive (headless) when no backend can drive this machine.
#if defined(RECREATION_RHI_D3D12) && defined(_WIN32)
      device = d3d12::CreateD3D12Device(desc, window);
#endif
#if defined(RECREATION_RHI_VULKAN)
      if (!device) device = vk::CreateVulkanDevice(desc, window);
#endif
      break;
  }
  if (!device) {
    if (desc.backend != Backend::kNull) {
      REC_WARN("no {} gpu backend available, renderer disabled",
               BackendName(desc.backend));
    }
    device = null::CreateNullDevice();
  }
  return device;
}

u32 FormatTexelBytes(Format format) {
  switch (format) {
    case Format::kR8Unorm:
    case Format::kR8Snorm:
    case Format::kR8Uint:
      return 1;
    case Format::kRG8Unorm:
    case Format::kR16Unorm:
    case Format::kR16Snorm:
    case Format::kR16Uint:
    case Format::kR16Float:
      return 2;
    case Format::kRGBA8Unorm:
    case Format::kRGBA8Snorm:
    case Format::kRGBA8Uint:
    case Format::kRGBA8Srgb:
    case Format::kBGRA8Unorm:
    case Format::kRG16Unorm:
    case Format::kRG16Snorm:
    case Format::kRG16Float:
    case Format::kR32Uint:
    case Format::kR32Float:
    case Format::kRGB10A2Unorm:
    case Format::kRG11B10Float:
    case Format::kRGB9E5Float:
    case Format::kD32Float:
      return 4;
    case Format::kRGBA16Unorm:
    case Format::kRGBA16Snorm:
    case Format::kRGBA16Float:
    case Format::kRG32Float:
    case Format::kBC1RgbUnorm:  // 8 bytes per 4x4 block
    case Format::kBC1RgbSrgb:
    case Format::kBC4Unorm:
      return 8;
    case Format::kRGB32Float:
      return 12;
    case Format::kRGBA32Float:
    case Format::kBC2Unorm:  // 16 bytes per 4x4 block
    case Format::kBC2Srgb:
    case Format::kBC3Unorm:
    case Format::kBC3Srgb:
    case Format::kBC5Unorm:
    case Format::kBC7Unorm:
    case Format::kBC7Srgb:
      return 16;
    case Format::kUnknown:
    case Format::kCount:
      break;
  }
  return 4;
}

}  // namespace rec::render

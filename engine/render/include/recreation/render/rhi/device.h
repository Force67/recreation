#ifndef RECREATION_RENDER_RHI_DEVICE_H_
#define RECREATION_RENDER_RHI_DEVICE_H_

#include <memory>
#include <string>

#include "recreation/core/types.h"
#include "recreation/core/window.h"

namespace rec::render {

struct DeviceDesc {
  bool enable_validation = false;
  bool request_raytracing = true;
};

// What the physical device actually supports. Raytracing and friends are
// queried, never assumed, so the same binary runs on a desktop GPU and an
// android phone.
struct DeviceCaps {
  std::string adapter_name;
  bool raytracing = false;
  bool mesh_shaders = false;
  bool fragment_shading_rate = false;
};

// Thin ownership wrapper over instance, physical device, logical device and
// queues. Compiles to a stub without the Vulkan SDK so the headless server
// and CI builds need no GPU stack.
class Device {
 public:
  static std::unique_ptr<Device> Create(const DeviceDesc& desc, const NativeWindowHandles& window);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const DeviceCaps& caps() const { return caps_; }
  bool is_stub() const { return is_stub_; }

  void WaitIdle();

 private:
  Device() = default;

  DeviceCaps caps_;
  bool is_stub_ = true;

  struct VulkanState;
  std::unique_ptr<VulkanState> vk_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_DEVICE_H_

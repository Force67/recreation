#ifndef RECREATION_RENDER_FRAME_GENERATION_H_
#define RECREATION_RENDER_FRAME_GENERATION_H_

// FSR3 frame generation without the SDK's proxy swapchain: optical flow +
// frame interpolation dispatched at the end of the frame's command list,
// producing the midpoint between the previous and the current backbuffer.
// Requires the FSR3 upscaler (its dilated depth/motion guides are reused).
// The renderer copies it into a second acquired swapchain image and presents
// it before the real frame (Device::SubmitFrameGen); under FIFO the two
// presents land a vblank apart, which is the whole pacing story.

#include <memory>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

struct FrameGenDesc {
  u32 display_width = 0;
  u32 display_height = 0;
  u32 render_width = 0;   // depth/motion resolution
  u32 render_height = 0;
};

struct FrameGenInputs {
  const GpuImage* backbuffer = nullptr;  // presented color, display res, kShaderReadCompute
  // The FSR3 upscaler's shared resources (it dilates depth/motion every
  // frame); reusing them keeps frame interpolation at one dispatch per
  // context per frame, inside the ffx backend's view-recycling window.
  const GpuImage* dilated_depth = nullptr;
  const GpuImage* dilated_motion = nullptr;
  const GpuImage* recon_prev_depth = nullptr;
  f32 frame_delta_seconds = 0;
  f32 camera_near = 0.1f;
  f32 camera_fov_y = 1.0472f;
  u64 frame_id = 0;  // must increment by exactly one per rendered frame
  bool reset = false;
};

class FrameGenerator {
 public:
  virtual ~FrameGenerator() = default;

  // Records optical flow + interpolation into cmd. The interpolated image is
  // left in kGeneral; the caller copies it to the second swapchain image.
  // Returns false when a dispatch failed (present the real frame only).
  virtual bool Record(CommandList& cmd, const FrameGenInputs& inputs) = 0;
  virtual const GpuImage& interpolated() const = 0;
  // Pre-UI copy of the backbuffer (kShaderReadCompute between frames): the
  // renderer blits into it right before the ui pass so interpolation sources
  // HUD-less data, and the UI is re-drawn crisp onto the generated frame.
  virtual const GpuImage& hudless() const = 0;
};

// Returns null when FSR3 is not compiled in or the device is not vulkan.
std::unique_ptr<FrameGenerator> CreateFrameGenerator(Device& device, const FrameGenDesc& desc);

}  // namespace rec::render

#endif  // RECREATION_RENDER_FRAME_GENERATION_H_

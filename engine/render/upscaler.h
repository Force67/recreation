#ifndef RECREATION_RENDER_UPSCALER_H_
#define RECREATION_RENDER_UPSCALER_H_

#include <memory>

#include "core/types.h"
#include "render/render_graph.h"

namespace rec::render {

enum class UpscalerKind : u8 { kNone, kFsr3, kDlss, kXess };

struct UpscalerDesc {
  UpscalerKind kind = UpscalerKind::kNone;
  u32 render_width = 0;
  u32 render_height = 0;
  u32 output_width = 0;
  u32 output_height = 0;
  f32 sharpness = 0.0f;
};

struct UpscalerInputs {
  // Resource handles into the render graph, filled by the renderer.
  ResourceHandle color = kInvalidResource;
  ResourceHandle depth = kInvalidResource;
  ResourceHandle motion_vectors = kInvalidResource;
  ResourceHandle exposure = kInvalidResource;
  f32 jitter_x = 0;  // pixel units, the same value baked into the projection
  f32 jitter_y = 0;
  f32 sharpness = 0;
  f32 frame_delta_seconds = 0;
  f32 camera_near = 0.1f;
  f32 camera_fov_y = 1.0472f;
  bool reset_history = false;
};

// One implementation per vendor SDK. Each lives behind this boundary so the
// renderer stays free of vendor headers.
class Upscaler {
 public:
  virtual ~Upscaler() = default;

  virtual bool Initialize(const UpscalerDesc& desc) = 0;
  // Adds the upscale pass and returns the handle of the full resolution
  // output, or kInvalidResource on failure (renderer shows the input then).
  virtual ResourceHandle AddToGraph(RenderGraph& graph, const UpscalerInputs& inputs) = 0;
  virtual UpscalerKind kind() const = 0;
};

// Returns null if the SDK for the requested kind is not compiled in or the
// device does not support it. Caller falls back to TAA.
std::unique_ptr<Upscaler> CreateUpscaler(const UpscalerDesc& desc, class Device& device);

}  // namespace rec::render

#endif  // RECREATION_RENDER_UPSCALER_H_

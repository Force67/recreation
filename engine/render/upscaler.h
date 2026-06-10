#ifndef RECREATION_RENDER_UPSCALER_H_
#define RECREATION_RENDER_UPSCALER_H_

#include <memory>

#include "core/types.h"

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
  u32 color = 0;
  u32 depth = 0;
  u32 motion_vectors = 0;
  u32 exposure = 0;
  f32 jitter_x = 0;
  f32 jitter_y = 0;
  bool reset_history = false;
};

// One implementation per vendor SDK. Each lives behind this boundary so the
// renderer stays free of vendor headers.
class Upscaler {
 public:
  virtual ~Upscaler() = default;

  virtual bool Initialize(const UpscalerDesc& desc) = 0;
  virtual void AddToGraph(class RenderGraph& graph, const UpscalerInputs& inputs) = 0;
  virtual UpscalerKind kind() const = 0;
};

// Returns null if the SDK for the requested kind is not compiled in or the
// device does not support it. Caller falls back to TAA.
std::unique_ptr<Upscaler> CreateUpscaler(const UpscalerDesc& desc, class Device& device);

}  // namespace rec::render

#endif  // RECREATION_RENDER_UPSCALER_H_

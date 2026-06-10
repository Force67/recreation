#ifndef RECREATION_RENDER_RENDERER_H_
#define RECREATION_RENDER_RENDERER_H_

#include <memory>

#include "core/window.h"
#include "ecs/world.h"
#include "render/antialiasing.h"
#include "render/raytracing.h"
#include "render/render_graph.h"
#include "render/rhi/device.h"
#include "render/upscaler.h"

namespace rec::render {

struct RendererDesc {
  bool enable_validation = false;
  AntiAliasingMode aa_mode = AntiAliasingMode::kTaa;
  UpscalerKind upscaler = UpscalerKind::kNone;
  RayTracingSettings raytracing;
  bool enable_raytracing = true;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  bool Initialize(const RendererDesc& desc, Window& window);
  void RenderFrame(ecs::World& world, f32 interpolation_alpha);
  void Shutdown();

  // Switching AA or upscaler at runtime resets temporal history.
  void SetAntiAliasing(AntiAliasingMode mode);
  void SetUpscaler(UpscalerKind kind);

  const DeviceCaps* caps() const;

 private:
  void BuildFrameGraph();

  RendererDesc desc_;
  std::unique_ptr<Device> device_;
  std::unique_ptr<Upscaler> upscaler_;
  std::unique_ptr<RayTracingContext> raytracing_;
  RenderGraph graph_;
  TaaPass taa_;
  u32 frame_index_ = 0;
  u32 render_width_ = 0;
  u32 render_height_ = 0;
  u32 output_width_ = 0;
  u32 output_height_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDERER_H_

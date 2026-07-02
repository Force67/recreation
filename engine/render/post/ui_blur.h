#ifndef RECREATION_RENDER_UI_BLUR_H_
#define RECREATION_RENDER_UI_BLUR_H_

#include <memory>

#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

// Produces a small Gaussian-blurred copy of the post-tonemap backbuffer for the
// UI's frosted-glass panels (CSS backdrop-blur). Two separable fullscreen
// passes (horizontal then vertical) at quarter resolution; the result is bound
// by the UI backend and sampled in screen space by frosted quads.
class UiBlurPass {
 public:
  static std::unique_ptr<UiBlurPass> Create(Device& device);
  ~UiBlurPass();

  UiBlurPass(const UiBlurPass&) = delete;
  UiBlurPass& operator=(const UiBlurPass&) = delete;

  // Adds the blur passes reading `src` (the backbuffer) and returns the blurred
  // texture handle. width/height are the full backbuffer dimensions.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle src, u32 width, u32 height);

  // Linear clamp sampler the UI backend uses to read the blurred result.
  SamplerHandle sampler() const { return sampler_; }

 private:
  explicit UiBlurPass(Device& device) : device_(device) {}
  void Record(PassContext& ctx, TextureView input, TextureView output, Extent2D extent,
              float dx, float dy);

  Device& device_;
  SamplerHandle sampler_;
  PipelineHandle pipeline_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_UI_BLUR_H_

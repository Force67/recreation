#ifndef RECREATION_RENDER_ANTIALIASING_H_
#define RECREATION_RENDER_ANTIALIASING_H_

#include "core/types.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

enum class AntiAliasingMode : u8 {
  kNone,
  kTaa,
  // Upscalers do their own temporal accumulation, TAA must be off when one
  // is active. The renderer enforces this.
  kUpscaler,
};

struct JitterSequence {
  // Halton (2,3) offsets in pixel units, centered around zero.
  static void Sample(u32 frame_index, u32 sample_count, f32* out_x, f32* out_y);
};

// Compute resolve with reprojection and 3x3 neighborhood clamping. Owns two
// persistent history images and ping pongs between them: the one written
// this frame is both the resolved output and next frame's history.
class TaaPass {
 public:
  struct Settings {
    f32 history_blend = 0.9f;
    u32 jitter_sample_count = 8;
  };

  bool Initialize(Device& device);
  void Resize(Device& device, VkExtent2D extent);
  void Destroy(Device& device);

  void Configure(const Settings& settings) { settings_ = settings; }
  void Reset() { history_valid_ = false; }

  // Adds the resolve pass and returns the handle of the resolved output.
  // debug_mode replaces the output with a debug visualization to a side target:
  // 1 = disocclusion heatmap (history rejection), 2 = motion vectors.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle motion,
                            u32 frame_index, u32 debug_mode = 0);

  const Settings& settings() const { return settings_; }

 private:
  Settings settings_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  GpuImage history_[2];
  VkImageLayout history_layouts_[2] = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};
  VkExtent2D extent_{};
  bool history_valid_ = false;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_ANTIALIASING_H_

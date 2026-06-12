#ifndef RECREATION_RENDER_POST_H_
#define RECREATION_RENDER_POST_H_

#include <memory>

#include "render/render_graph.h"
#include "render/rhi/device.h"
#include "render/settings.h"

namespace rec::render {

// Final pass of the frame: samples the resolved hdr image, tonemaps,
// applies the srgb transfer function and writes the backbuffer. Any gap
// between render and output resolution is absorbed here by the linear
// sampler until a real upscaler owns it.
class PostPass {
 public:
  static std::unique_ptr<PostPass> Create(Device& device, VkFormat output_format);
  ~PostPass();

  PostPass(const PostPass&) = delete;
  PostPass& operator=(const PostPass&) = delete;

  struct Params {
    f32 exposure = 1.0f;
    u32 tonemap = 0;  // TonemapOperator
  };

  void Record(PassContext& ctx, VkImageView input, VkImageView output, VkExtent2D output_extent,
              const Params& params);

 private:
  explicit PostPass(Device& device) : device_(device) {}

  Device& device_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_POST_H_

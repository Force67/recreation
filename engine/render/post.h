#ifndef RECREATION_RENDER_POST_H_
#define RECREATION_RENDER_POST_H_

#include <memory>

#include "render/render_graph.h"
#include "render/rhi/device.h"

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

  void Record(PassContext& ctx, VkImageView input, VkImageView output, VkExtent2D output_extent);

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

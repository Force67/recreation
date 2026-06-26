#ifndef RECREATION_RENDER_BLOOM_H_
#define RECREATION_RENDER_BLOOM_H_

#include "core/types.h"
#include "render/core/render_graph.h"

namespace rec::render {

class Device;

// Threshold-free physically based bloom: 13-tap Karis downsample chain into
// tent upsamples (CoD: Advanced Warfare). The tonemap pass mixes the result
// by a small weight instead of clipping a threshold.
class BloomPass {
 public:
  static constexpr u32 kMips = 6;

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Builds the chain off `input` (output resolution hdr) and returns the
  // full resolution bloom texture.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle input, u32 width, u32 height);

 private:
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline down_pipeline_ = VK_NULL_HANDLE;
  VkPipeline up_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_BLOOM_H_

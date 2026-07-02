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
  SamplerHandle sampler_;
  PipelineHandle down_pipeline_;
  PipelineHandle up_pipeline_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_BLOOM_H_

#ifndef RECREATION_RENDER_EXPOSURE_H_
#define RECREATION_RENDER_EXPOSURE_H_

#include "core/types.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Eye adaptation: 256 bin log luminance histogram reduced to a mean, then
// an exponential blend toward the keyed exposure. The result lives in a
// small storage buffer the tonemap pass reads on the gpu, no readback.
class ExposurePass {
 public:
  struct Settings {
    bool automatic = true;
    f32 compensation = 1.0f;      // multiplier on the metered exposure
    f32 adaptation_speed = 3.0f;  // 1/s
    f32 manual_exposure = 1.0f;   // used when automatic is off
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);
  void Configure(const Settings& settings) { settings_ = settings; }

  // Meters `input` and updates the exposure buffer with internal barriers.
  void AddToGraph(RenderGraph& graph, ResourceHandle input, u32 width, u32 height,
                  f32 delta_seconds);

  const GpuBuffer& exposure_buffer() const { return exposure_; }
  u64 exposure_buffer_size() const { return exposure_.size; }

 private:
  Settings settings_;
  Device* device_ = nullptr;
  SamplerHandle sampler_;
  GpuBuffer histogram_;  // 256 u32, cleared by the resolve pass each frame
  GpuBuffer exposure_;   // [0] exposure, [1] avg luma
  PipelineHandle histogram_pipeline_;
  PipelineHandle resolve_pipeline_;
  bool first_frame_ = true;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_EXPOSURE_H_

#ifndef RECREATION_RENDER_RAYTRACING_H_
#define RECREATION_RENDER_RAYTRACING_H_

#include "recreation/core/types.h"

namespace rec::render {

class Device;
class RenderGraph;

struct RayTracingSettings {
  bool shadows = true;
  bool reflections = false;
  bool global_illumination = false;
};

// Owns acceleration structures. Only constructed when DeviceCaps::raytracing
// is true, every pass it adds degrades to the raster path otherwise.
class RayTracingContext {
 public:
  explicit RayTracingContext(Device& device) : device_(device) {}

  void Configure(const RayTracingSettings& settings) { settings_ = settings; }

  // TODO: BLAS build per mesh asset, TLAS rebuild per frame from visible
  // instances, then shadow/reflection passes into the graph.
  void BuildAccelerationStructures();
  void AddPasses(RenderGraph& graph);

  const RayTracingSettings& settings() const { return settings_; }

 private:
  Device& device_;
  RayTracingSettings settings_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RAYTRACING_H_

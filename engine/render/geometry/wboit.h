#ifndef RECREATION_RENDER_WBOIT_H_
#define RECREATION_RENDER_WBOIT_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// One transparent instance for the order-independent transparency pass.
struct WboitInstance {
  Mat4 model;
  f32 color[4] = {1, 1, 1, 0.5f};  // rgb tint, a = alpha
};

// Weighted-blended order-independent transparency. Transparent geometry is
// accumulated into a colour/weight target and a transmittance target with
// commutative blends, then resolved over the opaque scene, so overlapping
// transparency needs no sorting. Owns a sphere and renders the supplied
// instances; a demo (--demo oit) of the technique that does not depend on the
// fragile material shaders.
class WboitPass {
 public:
  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);

  // Renders the instances order-independently and composites over color; returns
  // the composited colour handle.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                            const base::Vector<WboitInstance>& instances, const Mat4& view_proj,
                            const Vec3& sun_dir, const Vec3& sun_color, f32 ambient, u32 width,
                            u32 height);

 private:
  PipelineHandle geom_pipeline_;
  PipelineHandle resolve_pipeline_;
  SamplerHandle sampler_;
  GpuBuffer vertices_;
  GpuBuffer indices_;
  u32 index_count_ = 0;
  Format color_format_ = Format::kUnknown;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_WBOIT_H_

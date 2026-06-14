#ifndef RECREATION_RENDER_FUR_H_
#define RECREATION_RENDER_FUR_H_

#include "core/math.h"
#include "render/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Shell-based hair/fur on the raster path: draws an owned sphere as N concentric
// shells pushed out along the normal, and carves tapering hair strands out of
// each shell in the pixel shader. Renders over the lit scene, depth-tested
// against the prepass depth so a solid core occludes the far-side shells. A
// self-contained demo of the fur/hair technique (--demo fur).
class FurPass {
 public:
  struct Params {
    f32 fur_length = 0.14f;  // total fur height in model units
    u32 shell_count = 28;
    f32 base_color[3] = {0.46f, 0.32f, 0.17f};  // warm brown coat
  };

  bool Initialize(Device& device, VkFormat color_format, VkFormat depth_format);
  void Destroy(Device& device);

  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth, const Mat4& model,
                  const Mat4& view_proj, const Vec3& sun_dir, const Vec3& sun_color, f32 ambient,
                  const Params& params);

  f32 sphere_radius() const { return radius_; }

 private:
  static constexpr f32 radius_ = 1.0f;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  GpuBuffer vertices_;
  GpuBuffer indices_;
  u32 index_count_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_FUR_H_

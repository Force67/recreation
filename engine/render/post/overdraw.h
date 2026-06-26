#ifndef RECREATION_RENDER_OVERDRAW_H_
#define RECREATION_RENDER_OVERDRAW_H_

#include <functional>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

// Overdraw debug view. Re-renders the scene geometry with additive blending and
// no depth test into the resolved color (cleared first), so overlapping layers
// accumulate into a warm heat ramp. Reuses shadow.vs for the mvp transform; the
// caller emits the same opaque + transparent draws it would otherwise shade.
class OverdrawPass {
 public:
  bool Initialize(Device& device, VkFormat color_format);
  void Destroy(Device& device);

  // Clears `color_view` and additive-renders the geometry. draw is invoked with
  // the pipeline layout bound; it pushes each mesh's model matrix (offset 64)
  // and issues the draws. view_proj is pushed at offset 0.
  void Render(VkCommandBuffer cmd, VkImageView color_view, VkExtent2D extent, const Mat4& view_proj,
              const std::function<void(VkCommandBuffer, VkPipelineLayout)>& draw);

 private:
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_OVERDRAW_H_

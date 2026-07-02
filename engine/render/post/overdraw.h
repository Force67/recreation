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
  bool Initialize(Device& device, Format color_format);
  void Destroy(Device& device);

  // Clears `color_view` and additive-renders the geometry. draw is invoked with
  // the pipeline bound; for each mesh it pushes the full {view_proj, model}
  // 128-byte block (push constants always start at offset 0 in the RHI) and
  // issues the draws. view_proj is also pushed up front.
  void Render(CommandList& cmd, TextureView color_view, Extent2D extent, const Mat4& view_proj,
              const std::function<void(CommandList&)>& draw);

 private:
  PipelineHandle pipeline_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_OVERDRAW_H_

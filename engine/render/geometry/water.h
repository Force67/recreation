#ifndef RECREATION_RENDER_WATER_H_
#define RECREATION_RENDER_WATER_H_

#include <memory>

#include "render/core/bindless.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

// Water surface pipeline: fbm wave normals, raytraced reflections shaded
// through the bindless tables, screen space refraction with absorption
// against a snapshot of the opaque pass. Needs ray query; without it water
// falls back to the generic blend pipeline upstream.
class WaterPass {
 public:
  // Set layouts mirror the water.ps bindings: 0 mesh globals (+tlas),
  // 1 material, 2 environment, 3 bindless, 4 the opaque snapshot.
  static std::unique_ptr<WaterPass> Create(Device& device, Format color_format,
                                           Format motion_format, Format depth_format,
                                           BindingLayoutHandle globals_layout,
                                           BindingLayoutHandle material_layout,
                                           BindingLayoutHandle environment_layout,
                                           BindingLayoutHandle bindless_layout);
  ~WaterPass();

  WaterPass(const WaterPass&) = delete;
  WaterPass& operator=(const WaterPass&) = delete;

  // Compute copy of the opaque scene color into the snapshot transient.
  void RecordCopy(PassContext& ctx, ResourceHandle scene_color, ResourceHandle opaque_color,
                  u32 width, u32 height);

  // Binds the water pipeline; sets 0-2 are the frame's globals/material-less
  // environment sets, set 4 is written from the snapshot views.
  void Bind(PassContext& ctx, BindingSetHandle globals, BindingSetHandle environment,
            BindingSetHandle bindless, ResourceHandle opaque_color, ResourceHandle opaque_depth);
  void BindMaterial(CommandList& cmd, BindingSetHandle material);

 private:
  explicit WaterPass(Device& device) : device_(device) {}

  Device& device_;
  SamplerHandle sampler_;
  PipelineHandle pipeline_;
  PipelineHandle copy_pipeline_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_WATER_H_

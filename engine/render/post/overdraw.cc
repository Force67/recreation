#include "render/post/overdraw.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "shaders/overdraw_ps_hlsl.h"
#include "shaders/shadow_vs_hlsl.h"

namespace rec::render {

bool OverdrawPass::Initialize(Device& device, Format color_format) {
  // shadow.vs pushes {view_proj, model}; this pass reuses that 128-byte range.
  // shadow.vs reads position (0) and uv (3); supply both from the vertex buffer.
  // TODO(rhi): blend preset mismatch: old alpha factors were srcAlpha=ZERO,
  // dstAlpha=ONE (keep dst alpha); kAdditive is ONE/ONE on alpha too. Color
  // factors (ONE/ONE) match; the heatmap only reads rgb.
  pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_shadow_vs_hlsl),
      .fragment = REC_SHADER(k_overdraw_ps_hlsl),
      .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                          .attributes = {{.location = 0,
                                          .format = Format::kRGB32Float,
                                          .offset = offsetof(asset::Vertex, position)},
                                         {.location = 3,
                                          .format = Format::kRG32Float,
                                          .offset = offsetof(asset::Vertex, uv)}}}},
      .raster = {.cull = CullMode::kNone},  // count every overlapping layer
      .depth = {},                          // no test/write, no depth attachment
      .color_formats = {color_format},
      .blend = {BlendMode::kAdditive},  // additive accumulation
      .push_constant_size = 2 * sizeof(Mat4),
      .debug_name = "overdraw",
  });
  if (!pipeline_) {
    REC_ERROR("overdraw pipeline creation failed");
    return false;
  }
  return true;
}

void OverdrawPass::Render(CommandList& cmd, TextureView color_view, Extent2D extent,
                          const Mat4& view_proj,
                          const std::function<void(CommandList&)>& draw) {
  ColorAttachment color{.view = color_view,
                        .load = LoadOp::kClear,  // start from black, then accumulate
                        .store = StoreOp::kStore,
                        .clear = {0.0f, 0.0f, 0.0f, 1.0f}};
  cmd.BeginRendering({.extent = extent, .colors = {&color, 1}});
  cmd.BindPipeline(pipeline_);
  cmd.PushConstants(&view_proj, sizeof(Mat4));
  draw(cmd);
  cmd.EndRendering();
}

void OverdrawPass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

}  // namespace rec::render

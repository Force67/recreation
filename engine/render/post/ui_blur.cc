#include "render/post/ui_blur.h"

#include <algorithm>

#include "core/log.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/ui_blur_ps_hlsl.h"

namespace rec::render {

namespace {
constexpr Format kFrostFormat = Format::kRGBA16Float;
struct BlurPush {
  float dir[2];  // per-tap UV step along one axis
};
}  // namespace

std::unique_ptr<UiBlurPass> UiBlurPass::Create(Device& device) {
  auto pass = std::unique_ptr<UiBlurPass>(new UiBlurPass(device));

  pass->sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                      .address_v = AddressMode::kClampToEdge,
                                      .address_w = AddressMode::kClampToEdge});

  pass->pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_fullscreen_vs_hlsl),
      .fragment = REC_SHADER(k_ui_blur_ps_hlsl),
      .raster = {.cull = CullMode::kNone},
      .color_formats = {kFrostFormat},
      .blend = {BlendMode::kOpaque},
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler}},
                .stages = kShaderStageFragment}},
      .push_constant_size = sizeof(BlurPush),
      .debug_name = "ui_blur",
  });
  if (!pass->pipeline_) {
    REC_ERROR("ui_blur pipeline creation failed");
    return nullptr;
  }
  return pass;
}

UiBlurPass::~UiBlurPass() { device_.DestroyPipeline(pipeline_); }

void UiBlurPass::Record(PassContext& ctx, TextureView input, TextureView output, Extent2D extent,
                        float dx, float dy) {
  ColorAttachment color{.view = output, .load = LoadOp::kDontCare, .store = StoreOp::kStore};
  ctx.cmd->BeginRendering({.extent = extent, .colors = {&color, 1}});
  ctx.cmd->BindPipeline(pipeline_);
  ctx.cmd->BindTransient(0, {Bind::Combined(0, input, sampler_)});
  BlurPush push{{dx, dy}};
  ctx.cmd->Push(push);
  ctx.cmd->Draw(3);
  ctx.cmd->EndRendering();
}

ResourceHandle UiBlurPass::AddToGraph(RenderGraph& graph, ResourceHandle src, u32 width,
                                      u32 height) {
  const u32 dw = std::max(1u, width / 4);
  const u32 dh = std::max(1u, height / 4);
  ResourceHandle h_blur =
      graph.CreateTexture({.name = "ui_frost_h", .format = kFrostFormat, .width = dw, .height = dh});
  ResourceHandle frost =
      graph.CreateTexture({.name = "ui_frost", .format = kFrostFormat, .width = dw, .height = dh});

  const float step_x = 1.0f / static_cast<float>(dw);
  const float step_y = 1.0f / static_cast<float>(dh);
  const Extent2D extent{dw, dh};

  graph.AddPass(
      "ui_blur_h",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(src, ResourceUsage::kSampledFragment);
        builder.Write(h_blur, ResourceUsage::kColorAttachment);
      },
      [this, src, h_blur, extent, step_x](PassContext& ctx) {
        Record(ctx, ctx.graph->image(src).view, ctx.graph->image(h_blur).view, extent, step_x, 0.0f);
      });

  graph.AddPass(
      "ui_blur_v",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(h_blur, ResourceUsage::kSampledFragment);
        builder.Write(frost, ResourceUsage::kColorAttachment);
      },
      [this, h_blur, frost, extent, step_y](PassContext& ctx) {
        Record(ctx, ctx.graph->image(h_blur).view, ctx.graph->image(frost).view, extent, 0.0f,
               step_y);
      });

  return frost;
}

}  // namespace rec::render

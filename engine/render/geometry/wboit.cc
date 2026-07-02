#include "render/geometry/wboit.h"

#include "asset/primitives.h"
#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/wboit_ps_hlsl.h"
#include "shaders/wboit_resolve_ps_hlsl.h"
#include "shaders/wboit_vs_hlsl.h"

namespace rec::render {
namespace {

constexpr Format kAccumFormat = Format::kRGBA16Float;
constexpr Format kRevealFormat = Format::kR16Float;

struct WboitPush {
  Mat4 view_proj;
  Mat4 model;
  f32 color[4];
  f32 sun_dir[3];
  f32 pad0;
  f32 sun_color[3];
  f32 ambient;
};

}  // namespace

bool WboitPass::Initialize(Device& device, Format color_format, Format depth_format) {
  color_format_ = color_format;
  asset::Mesh sphere = asset::MakeSphere(1.0f, 40, 60, asset::MakeAssetId("builtin/oit/sphere"));
  const asset::MeshLod& lod = sphere.lods[0];
  index_count_ = static_cast<u32>(lod.indices.size());
  vertices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.vertices.data()),
               lod.vertices.size() * sizeof(asset::Vertex)),
      kBufferUsageVertex);
  indices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.indices.data()), lod.indices.size() * sizeof(u32)),
      kBufferUsageIndex);

  // --- Geometry pipeline: accumulate into the two oit targets. ---
  // accum: additive. revealage: dst *= (1 - src.r).
  geom_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_wboit_vs_hlsl),
      .fragment = REC_SHADER(k_wboit_ps_hlsl),
      .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                          .attributes = {{0, Format::kRGB32Float,
                                          offsetof(asset::Vertex, position)},
                                         {1, Format::kRGB32Float,
                                          offsetof(asset::Vertex, normal)}}}},
      .raster = {.cull = CullMode::kNone,  // see through both faces
                 .front = FrontFace::kCounterClockwise},
      .depth = {.test = true,
                .write = false,
                .compare = CompareOp::kGreaterEqual,  // reversed z, occluded by opaque
                .format = depth_format},
      .color_formats = {kAccumFormat, kRevealFormat},
      .blend = {BlendMode::kWboitAccum, BlendMode::kWboitReveal},
      .push_constant_size = sizeof(WboitPush),
      .debug_name = "wboit_geom",
  });
  if (!geom_pipeline_) {
    REC_ERROR("wboit geometry pipeline creation failed");
    return false;
  }

  // --- Resolve pipeline: composite the oit targets over the scene. ---
  sampler_ = device.GetSampler({.min_filter = Filter::kNearest,
                                .mag_filter = Filter::kNearest,
                                .mip_filter = Filter::kNearest,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge,
                                .address_w = AddressMode::kClampToEdge});

  resolve_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_fullscreen_vs_hlsl),
      .fragment = REC_SHADER(k_wboit_resolve_ps_hlsl),
      .raster = {.cull = CullMode::kNone, .front = FrontFace::kCounterClockwise},
      .color_formats = {color_format_},
      .blend = {BlendMode::kOpaque},
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler}},
                .stages = kShaderStageFragment}},
      .debug_name = "wboit_resolve",
  });
  if (!resolve_pipeline_) {
    REC_ERROR("wboit resolve pipeline creation failed");
    return false;
  }
  return true;
}

ResourceHandle WboitPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                     const base::Vector<WboitInstance>& instances,
                                     const Mat4& view_proj, const Vec3& sun_dir,
                                     const Vec3& sun_color, f32 ambient, u32 width, u32 height) {
  ResourceHandle accum =
      graph.CreateTexture({.name = "oit_accum", .format = kAccumFormat, .width = width, .height = height});
  ResourceHandle reveal =
      graph.CreateTexture({.name = "oit_reveal", .format = kRevealFormat, .width = width, .height = height});
  ResourceHandle composite = graph.CreateTexture(
      {.name = "oit_composite", .format = color_format_, .width = width, .height = height});

  WboitPush base{};
  base.view_proj = view_proj;
  base.sun_dir[0] = sun_dir.x;
  base.sun_dir[1] = sun_dir.y;
  base.sun_dir[2] = sun_dir.z;
  base.sun_color[0] = sun_color.x;
  base.sun_color[1] = sun_color.y;
  base.sun_color[2] = sun_color.z;
  base.ambient = ambient;

  graph.AddPass(
      "oit_accumulate",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(accum, ResourceUsage::kColorAttachment);
        builder.Write(reveal, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, accum, reveal, depth, instances, base, width, height](PassContext& ctx) {
        ColorAttachment colors[2];
        colors[0] = {.view = ctx.graph->image(accum).view,
                     .load = LoadOp::kClear,
                     .clear = {0.0f, 0.0f, 0.0f, 0.0f}};
        colors[1] = {.view = ctx.graph->image(reveal).view,
                     .load = LoadOp::kClear,
                     .clear = {1.0f, 0.0f, 0.0f, 0.0f}};  // full transmittance
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};

        ctx.cmd->BeginRendering(
            {.extent = {width, height}, .colors = colors, .depth = &depth_att});
        ctx.cmd->BindPipeline(geom_pipeline_);
        ctx.cmd->BindVertexBuffer(0, vertices_, 0);
        ctx.cmd->BindIndexBuffer(indices_, 0, IndexType::kUint32);
        for (const WboitInstance& inst : instances) {
          WboitPush push = base;
          push.model = inst.model;
          push.color[0] = inst.color[0];
          push.color[1] = inst.color[1];
          push.color[2] = inst.color[2];
          push.color[3] = inst.color[3];
          ctx.cmd->Push(push);
          ctx.cmd->DrawIndexed(index_count_, 1, 0, 0, 0);
        }
        ctx.cmd->EndRendering();
      });

  graph.AddPass(
      "oit_resolve",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(accum, ResourceUsage::kSampledFragment);
        builder.Read(reveal, ResourceUsage::kSampledFragment);
        builder.Read(color, ResourceUsage::kSampledFragment);
        builder.Write(composite, ResourceUsage::kColorAttachment);
      },
      [this, accum, reveal, color, composite, width, height](PassContext& ctx) {
        ColorAttachment att[] = {{.view = ctx.graph->image(composite).view,
                                  .load = LoadOp::kDontCare}};
        ctx.cmd->BeginRendering({.extent = {width, height}, .colors = att});
        ctx.cmd->BindPipeline(resolve_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Combined(0, ctx.graph->image(accum).view, sampler_),
                Bind::Combined(1, ctx.graph->image(reveal).view, sampler_),
                Bind::Combined(2, ctx.graph->image(color).view, sampler_)});
        ctx.cmd->Draw(3, 1, 0, 0);
        ctx.cmd->EndRendering();
      });
  return composite;
}

void WboitPass::Destroy(Device& device) {
  device.DestroyPipeline(geom_pipeline_);
  geom_pipeline_ = {};
  device.DestroyPipeline(resolve_pipeline_);
  resolve_pipeline_ = {};
  device.DestroyBuffer(vertices_);
  device.DestroyBuffer(indices_);
}

}  // namespace rec::render

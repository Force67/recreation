#include "render/geometry/water.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "render/pipeline/mesh_pipeline.h"
#include "shaders/copy_cs_hlsl.h"
#include "shaders/mesh_vs_hlsl.h"
#include "shaders/water_ps_hlsl.h"

namespace rec::render {

std::unique_ptr<WaterPass> WaterPass::Create(Device& device, Format color_format,
                                             Format motion_format, Format depth_format,
                                             BindingLayoutHandle globals_layout,
                                             BindingLayoutHandle material_layout,
                                             BindingLayoutHandle environment_layout,
                                             BindingLayoutHandle bindless_layout) {
  auto pass = std::unique_ptr<WaterPass>(new WaterPass(device));

  pass->sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                      .address_v = AddressMode::kClampToEdge,
                                      .address_w = AddressMode::kClampToEdge});

  // Water replaces its pixels (refraction samples the snapshot), so it
  // renders opaquely over the scene and writes depth for the post stack.
  pass->pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_mesh_vs_hlsl),
      .fragment = REC_SHADER(k_water_ps_hlsl),
      .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                          .attributes = {{0, Format::kRGB32Float,
                                          offsetof(asset::Vertex, position)},
                                         {1, Format::kRGB32Float, offsetof(asset::Vertex, normal)},
                                         {2, Format::kRGBA32Float,
                                          offsetof(asset::Vertex, tangent)},
                                         {3, Format::kRG32Float, offsetof(asset::Vertex, uv)},
                                         {4, Format::kRGBA8Unorm,
                                          offsetof(asset::Vertex, color)}}}},
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true,
                .write = true,
                .compare = CompareOp::kGreater,  // reversed z
                .format = depth_format},
      .color_formats = {color_format, motion_format},
      .blend = {BlendMode::kOpaque, BlendMode::kOpaque},
      .sets = {{.shared = globals_layout},
               {.shared = material_layout},
               {.shared = environment_layout},
               {.shared = bindless_layout},
               {.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kCombinedTextureSampler}},
                .stages = kShaderStageFragment}},
      .push_constant_size = sizeof(MeshPushConstants),
      .debug_name = "water",
  });
  if (!pass->pipeline_) {
    REC_ERROR("water pipeline creation failed");
    return nullptr;
  }

  // Snapshot copy compute.
  pass->copy_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_copy_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler}}}},
      .debug_name = "water_copy",
  });
  if (!pass->copy_pipeline_) {
    REC_ERROR("water copy pipeline creation failed");
    return nullptr;
  }
  return pass;
}

WaterPass::~WaterPass() {
  if (pipeline_) device_.DestroyPipeline(pipeline_);
  if (copy_pipeline_) device_.DestroyPipeline(copy_pipeline_);
}

void WaterPass::RecordCopy(PassContext& ctx, ResourceHandle scene_color,
                           ResourceHandle opaque_color, u32 width, u32 height) {
  ctx.cmd->BindPipeline(copy_pipeline_);
  ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(opaque_color)),
                             Bind::Combined(1, ctx.graph->image(scene_color).view, sampler_)});
  ctx.cmd->Dispatch2D({width, height});
}

void WaterPass::Bind(PassContext& ctx, BindingSetHandle globals, BindingSetHandle environment,
                     BindingSetHandle bindless, ResourceHandle opaque_color,
                     ResourceHandle opaque_depth) {
  ctx.cmd->BindPipeline(pipeline_);
  ctx.cmd->BindSet(0, globals);
  ctx.cmd->BindSet(2, environment);
  ctx.cmd->BindSet(3, bindless);
  ctx.cmd->BindTransient(4, {Bind::Combined(0, ctx.graph->image(opaque_color).view, sampler_),
                             Bind::Combined(1, ctx.graph->image(opaque_depth).view, sampler_)});
}

void WaterPass::BindMaterial(CommandList& cmd, BindingSetHandle material) {
  cmd.BindSet(1, material);
}

}  // namespace rec::render

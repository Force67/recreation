#include "render/pipeline/mesh_pipeline.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "shaders/mesh_ps_hlsl.h"
#include "shaders/mesh_rt_ps_hlsl.h"
#include "shaders/mesh_rt_sss_ps_hlsl.h"
#include "shaders/mesh_sss_ps_hlsl.h"
#include "shaders/mesh_scene_as_hlsl.h"
#include "shaders/mesh_scene_ms_hlsl.h"
#include "shaders/mesh_skin_vs_hlsl.h"
#include "shaders/mesh_vs_hlsl.h"
#include "shaders/prepass_ps_hlsl.h"

namespace rec::render {

std::unique_ptr<MeshPipeline> MeshPipeline::Create(Device& device, Format color_format,
                                                   Format motion_format,
                                                   Format normal_format, Format depth_format,
                                                   BindingLayoutHandle material_layout,
                                                   BindingLayoutHandle environment_layout,
                                                   BindingLayoutHandle bindless_layout) {
  auto pipeline = std::unique_ptr<MeshPipeline>(new MeshPipeline(device));
  bool rt = device.caps().ray_query;
  bool mesh_caps = device.caps().mesh_shaders;
  pipeline->has_bindless_ = static_cast<bool>(bindless_layout);

  // Set 0 (frame globals), created once and shared by every variant so
  // descriptor sets bound once serve them all. The mesh-shader path reads the
  // globals from the task and mesh stages too; binding 2 is last frame's hi-z,
  // sampled by the task stage for instance occlusion cull.
  BindingLayoutDesc set0{};
  set0.stages = kShaderStageVertex | kShaderStageFragment |
                (mesh_caps ? (kShaderStageTask | kShaderStageMesh) : 0u);
  set0.slots.push_back({0, BindingType::kUniformBuffer});
  if (rt) set0.slots.push_back({1, BindingType::kAccelStruct});
  if (mesh_caps) set0.slots.push_back({2, BindingType::kSampledImage});
  pipeline->set_layout_ = device.CreateBindingLayout(set0);
  if (!pipeline->set_layout_) return nullptr;

  // Shared set interface: 0 = frame globals, 1 = material, 2 = environment,
  // 3 = bindless (rt hit shading), mirroring the old pipeline layout.
  base::Vector<PipelineBindings> sets;
  sets.push_back({.shared = pipeline->set_layout_});
  sets.push_back({.shared = material_layout});
  sets.push_back({.shared = environment_layout});
  if (pipeline->has_bindless_) sets.push_back({.shared = bindless_layout});

  VertexBufferLayout static_stream{
      .stride = sizeof(asset::Vertex),
      .attributes = {{0, Format::kRGB32Float, offsetof(asset::Vertex, position)},
                     {1, Format::kRGB32Float, offsetof(asset::Vertex, normal)},
                     {2, Format::kRGBA32Float, offsetof(asset::Vertex, tangent)},
                     {3, Format::kRG32Float, offsetof(asset::Vertex, uv)},
                     {4, Format::kRGBA8Unorm, offsetof(asset::Vertex, color)}}};
  // Skinned variant: a second vertex stream (binding 1) carries 4 bone indices
  // and 4 normalized weights per vertex.
  VertexBufferLayout skin_stream{
      .stride = sizeof(asset::SkinnedVertexExtra),
      .attributes = {{5, Format::kRGBA8Uint, offsetof(asset::SkinnedVertexExtra, bone_indices)},
                     {6, Format::kRGBA8Unorm, offsetof(asset::SkinnedVertexExtra, bone_weights)}}};

  // The prepass owns depth; main variants test EQUAL against it and leave
  // motion alone (the prepass already wrote it).
  GraphicsPipelineDesc scene{};
  scene.vertex = REC_SHADER(k_mesh_vs_hlsl);
  scene.fragment = REC_SHADER(k_mesh_sss_ps_hlsl);
  scene.vertex_buffers = {static_stream};
  // TODO: back face culling once converted content settles winding order.
  scene.raster = {.cull = CullMode::kNone};
  scene.depth = {.test = true, .write = false, .compare = CompareOp::kEqual,
                 .format = depth_format};
  // Third target: skin diffuse export for the screen-space sss blur. The
  // scene-pass fragment variants (mesh_sss/mesh_rt_sss) write it; the blend
  // pass keeps the two-target shaders.
  scene.color_formats = {color_format, motion_format, kSkinDiffuseFormat};
  // TODO(rhi): blend preset mismatch: the motion target (attachment 1) had
  // colorWriteMask 0 (motion comes from the prepass); presets always write
  // RGBA. Closest is kOpaque.
  scene.blend = {BlendMode::kOpaque, BlendMode::kOpaque, BlendMode::kOpaque};
  scene.sets = sets;
  scene.push_constant_size = sizeof(MeshPushConstants);
  scene.debug_name = "mesh_scene";

  bool wire_capable = device.caps().fill_mode_non_solid;
  for (u32 variant = 0; variant < 4; ++variant) {
    if ((variant & kRt) && !rt) continue;
    if ((variant & kWire) && !wire_capable) continue;
    GraphicsPipelineDesc desc = scene;
    desc.fragment =
        (variant & kRt) ? REC_SHADER(k_mesh_rt_sss_ps_hlsl) : REC_SHADER(k_mesh_sss_ps_hlsl);
    desc.raster.polygon = (variant & kWire) ? PolygonMode::kLine : PolygonMode::kFill;
    pipeline->pipelines_[variant] = device.CreateGraphicsPipeline(desc);
    if (!pipeline->pipelines_[variant]) {
      REC_ERROR("mesh pipeline creation failed (variant {})", variant);
    }
  }

  // Skinned main variants: same fragment shaders and main pass state, skinned
  // vertex stage + the bone weight stream. Wireframe is not skinned.
  {
    GraphicsPipelineDesc desc = scene;
    desc.vertex = REC_SHADER(k_mesh_skin_vs_hlsl);
    desc.vertex_buffers = {static_stream, skin_stream};
    desc.debug_name = "mesh_scene_skinned";
    for (u32 variant = 0; variant < 2; ++variant) {
      if ((variant & kRt) && !rt) continue;
      desc.fragment =
          (variant & kRt) ? REC_SHADER(k_mesh_rt_sss_ps_hlsl) : REC_SHADER(k_mesh_sss_ps_hlsl);
      pipeline->skinned_pipelines_[variant] = device.CreateGraphicsPipeline(desc);
      if (!pipeline->skinned_pipelines_[variant]) {
        REC_ERROR("mesh skinned pipeline creation failed (variant {})", variant);
      }
    }
  }

  // Transparent variants: blend over the opaque pass, test against its
  // depth without writing, shade with the same pbr shaders.
  {
    GraphicsPipelineDesc desc = scene;
    desc.depth.compare = CompareOp::kGreater;  // reversed z
    // Two attachments only (no skin export in the transparent pass), so the
    // fragments below stay the plain two-target shaders.
    desc.color_formats = {color_format, motion_format};
    // TODO(rhi): blend preset mismatch: old alpha factors were ONE/ZERO;
    // kAlpha uses ONE/ONE_MINUS_SRC_ALPHA. Color factors (SRC_ALPHA,
    // ONE_MINUS_SRC_ALPHA) match; kAlpha is the closest preset.
    desc.blend = {BlendMode::kAlpha, BlendMode::kOpaque};
    desc.debug_name = "mesh_blend";
    for (u32 variant = 0; variant < 2; ++variant) {
      if (variant == 1 && !rt) continue;
      desc.fragment = variant == 1 ? REC_SHADER(k_mesh_rt_ps_hlsl) : REC_SHADER(k_mesh_ps_hlsl);
      pipeline->blend_pipelines_[variant] = device.CreateGraphicsPipeline(desc);
      if (!pipeline->blend_pipelines_[variant]) {
        REC_ERROR("mesh blend pipeline creation failed");
      }
    }
    // Additive (one, one) variant for HDR effect-shader fire/glows: same shaders
    // (the unlit branch premultiplies its coverage into the colour).
    desc.blend = {BlendMode::kAdditive, BlendMode::kOpaque};
    desc.debug_name = "mesh_blend_additive";
    for (u32 variant = 0; variant < 2; ++variant) {
      if (variant == 1 && !rt) continue;
      desc.fragment = variant == 1 ? REC_SHADER(k_mesh_rt_ps_hlsl) : REC_SHADER(k_mesh_ps_hlsl);
      pipeline->blend_additive_pipelines_[variant] = device.CreateGraphicsPipeline(desc);
      if (!pipeline->blend_additive_pipelines_[variant]) {
        REC_ERROR("mesh additive blend pipeline creation failed");
      }
    }
  }

  // Prepass: depth write + normals/motion/depth-export targets, same layout.
  {
    GraphicsPipelineDesc desc = scene;
    desc.fragment = REC_SHADER(k_prepass_ps_hlsl);
    desc.depth = {.test = true, .write = true, .compare = CompareOp::kGreater,  // reversed z
                  .format = depth_format};
    desc.color_formats = {normal_format, motion_format, Format::kR32Float};
    // TODO(rhi): blend preset mismatch: prepass targets wrote RG/RG/R color
    // masks; presets always write RGBA. Closest is kOpaque.
    desc.blend = {BlendMode::kOpaque, BlendMode::kOpaque, BlendMode::kOpaque};
    desc.debug_name = "mesh_prepass";
    pipeline->prepass_pipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline->prepass_pipeline_) {
      REC_ERROR("mesh prepass pipeline creation failed");
    }

    // Skinned prepass: must match the skinned main pose so the EQUAL depth test
    // passes.
    desc.vertex = REC_SHADER(k_mesh_skin_vs_hlsl);
    desc.vertex_buffers = {static_stream, skin_stream};
    desc.debug_name = "mesh_prepass_skinned";
    pipeline->skinned_prepass_pipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline->skinned_prepass_pipeline_) {
      REC_ERROR("mesh skinned prepass pipeline creation failed");
    }
  }

  // Optional mesh-shader opaque variants: the vertex stage is replaced by a
  // meshlet mesh shader (geometry fed by device address), sharing the same
  // descriptor set layouts and fragment shaders. The larger mesh-stage push
  // range rides in the per-variant pipeline descs.
  if (mesh_caps) {
    GraphicsPipelineDesc ms{};
    ms.task = REC_SHADER(k_mesh_scene_as_hlsl);
    ms.mesh = REC_SHADER(k_mesh_scene_ms_hlsl);
    ms.raster = {.cull = CullMode::kNone};  // matches the raster path's winding policy
    ms.sets = sets;
    ms.push_constant_size = sizeof(MeshShaderPush);

    // Scene variants: depth EQUAL against the prepass, lit color + masked
    // motion target, like the raster main variants.
    {
      GraphicsPipelineDesc desc = ms;
      desc.depth = {.test = true, .write = false, .compare = CompareOp::kEqual,
                    .format = depth_format};
      desc.color_formats = {color_format, motion_format, kSkinDiffuseFormat};
      // TODO(rhi): blend preset mismatch: motion target had colorWriteMask 0;
      // closest is kOpaque (see the raster scene variants).
      desc.blend = {BlendMode::kOpaque, BlendMode::kOpaque, BlendMode::kOpaque};
      desc.debug_name = "mesh_ms_scene";
      for (u32 variant = 0; variant < 2; ++variant) {
        if (variant == kRt && !rt) continue;
        desc.fragment =
            variant == kRt ? REC_SHADER(k_mesh_rt_sss_ps_hlsl) : REC_SHADER(k_mesh_sss_ps_hlsl);
        pipeline->ms_scene_[variant] = device.CreateGraphicsPipeline(desc);
        if (!pipeline->ms_scene_[variant]) {
          REC_ERROR("mesh-shader scene pipeline creation failed (variant {})", variant);
        }
      }
    }

    // Prepass variant: depth write + normal/motion/depth-export targets.
    {
      GraphicsPipelineDesc desc = ms;
      desc.fragment = REC_SHADER(k_prepass_ps_hlsl);
      desc.depth = {.test = true, .write = true, .compare = CompareOp::kGreater,  // reversed z
                    .format = depth_format};
      desc.color_formats = {normal_format, motion_format, Format::kR32Float};
      // TODO(rhi): blend preset mismatch: prepass targets wrote RG/RG/R color
      // masks; closest is kOpaque (see the raster prepass).
      desc.blend = {BlendMode::kOpaque, BlendMode::kOpaque, BlendMode::kOpaque};
      desc.debug_name = "mesh_ms_prepass";
      pipeline->ms_prepass_ = device.CreateGraphicsPipeline(desc);
      if (!pipeline->ms_prepass_) {
        REC_ERROR("mesh-shader prepass pipeline creation failed");
      }
    }
  }

  if (!pipeline->pipelines_[0] || !pipeline->prepass_pipeline_ ||
      !pipeline->blend_pipelines_[0]) {
    return nullptr;
  }
  return pipeline;
}

MeshPipeline::~MeshPipeline() {
  for (PipelineHandle pipeline : pipelines_) {
    if (pipeline) device_.DestroyPipeline(pipeline);
  }
  for (PipelineHandle pipeline : blend_pipelines_) {
    if (pipeline) device_.DestroyPipeline(pipeline);
  }
  for (PipelineHandle pipeline : blend_additive_pipelines_) {
    if (pipeline) device_.DestroyPipeline(pipeline);
  }
  for (PipelineHandle pipeline : skinned_pipelines_) {
    if (pipeline) device_.DestroyPipeline(pipeline);
  }
  if (skinned_prepass_pipeline_) device_.DestroyPipeline(skinned_prepass_pipeline_);
  if (prepass_pipeline_) device_.DestroyPipeline(prepass_pipeline_);
  for (PipelineHandle pipeline : ms_scene_) {
    if (pipeline) device_.DestroyPipeline(pipeline);
  }
  if (ms_prepass_) device_.DestroyPipeline(ms_prepass_);
  if (set_layout_) device_.DestroyBindingLayout(set_layout_);
}

void MeshPipeline::Bind(CommandList& cmd, BindingSetHandle globals,
                        BindingSetHandle environment, BindingSetHandle bindless, bool use_rt,
                        bool wireframe) {
  u32 variant = (use_rt ? kRt : 0) | (wireframe ? kWire : 0);
  if (!pipelines_[variant]) variant &= ~kWire;
  if (!pipelines_[variant]) variant = 0;
  cmd.BindPipeline(pipelines_[variant]);
  cmd.BindSet(0, globals);
  cmd.BindSet(2, environment);
  if (has_bindless_ && bindless) {
    cmd.BindSet(3, bindless);
  }
}

void MeshPipeline::BindBlend(CommandList& cmd, BindingSetHandle globals,
                             BindingSetHandle environment, BindingSetHandle bindless,
                             bool use_rt) {
  u32 variant = use_rt && blend_pipelines_[1] ? 1 : 0;
  cmd.BindPipeline(blend_pipelines_[variant]);
  cmd.BindSet(0, globals);
  cmd.BindSet(2, environment);
  if (has_bindless_ && bindless) {
    cmd.BindSet(3, bindless);
  }
}

void MeshPipeline::BindBlendAdditive(CommandList& cmd, BindingSetHandle globals,
                                     BindingSetHandle environment, BindingSetHandle bindless,
                                     bool use_rt) {
  u32 variant = use_rt && blend_additive_pipelines_[1] ? 1 : 0;
  cmd.BindPipeline(blend_additive_pipelines_[variant]);
  cmd.BindSet(0, globals);
  cmd.BindSet(2, environment);
  if (has_bindless_ && bindless) {
    cmd.BindSet(3, bindless);
  }
}

void MeshPipeline::BindPrepass(CommandList& cmd, BindingSetHandle globals,
                               BindingSetHandle environment) {
  cmd.BindPipeline(prepass_pipeline_);
  cmd.BindSet(0, globals);
  // mesh.vs statically uses the env set (fft-ocean displacement sample), so
  // every draw path through it must have set 2 bound, prepass included.
  if (environment) cmd.BindSet(2, environment);
}

void MeshPipeline::BindMaterial(CommandList& cmd, BindingSetHandle material) {
  cmd.BindSet(1, material);
}

void MeshPipeline::SetSkinned(CommandList& cmd, bool skinned, bool use_rt, bool wireframe) {
  if (skinned) {
    PipelineHandle p = skinned_pipelines_[use_rt ? kRt : 0];
    if (!p) p = skinned_pipelines_[0];
    if (p) cmd.BindPipeline(p);
    return;
  }
  u32 variant = (use_rt ? kRt : 0) | (wireframe ? kWire : 0);
  if (!pipelines_[variant]) variant &= ~kWire;
  if (!pipelines_[variant]) variant = 0;
  cmd.BindPipeline(pipelines_[variant]);
}

void MeshPipeline::SetPrepassSkinned(CommandList& cmd, bool skinned) {
  PipelineHandle p =
      skinned && skinned_prepass_pipeline_ ? skinned_prepass_pipeline_ : prepass_pipeline_;
  cmd.BindPipeline(p);
}

void MeshPipeline::Draw(CommandList& cmd, const GpuMesh& mesh, const MeshPushConstants& push) {
  cmd.Push(push);
  cmd.BindVertexBuffer(0, mesh.vertices);
  if (mesh.skinned && mesh.skinning) {
    cmd.BindVertexBuffer(1, mesh.skinning);
  }
  cmd.BindIndexBuffer(mesh.indices, 0, IndexType::kUint32);
}

void MeshPipeline::DrawSubmesh(CommandList& cmd, const GpuSubmesh& submesh) {
  cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
}

void MeshPipeline::BindMeshScene(CommandList& cmd, BindingSetHandle globals,
                                 BindingSetHandle environment, BindingSetHandle bindless,
                                 bool use_rt) {
  PipelineHandle p = (use_rt && ms_scene_[kRt]) ? ms_scene_[kRt] : ms_scene_[0];
  cmd.BindPipeline(p);
  cmd.BindSet(0, globals);
  cmd.BindSet(2, environment);
  if (has_bindless_ && bindless) {
    cmd.BindSet(3, bindless);
  }
}

void MeshPipeline::BindMeshPrepass(CommandList& cmd, BindingSetHandle globals) {
  cmd.BindPipeline(ms_prepass_);
  cmd.BindSet(0, globals);
}

void MeshPipeline::BindMeshMaterial(CommandList& cmd, BindingSetHandle material) {
  cmd.BindSet(1, material);
}

void MeshPipeline::DrawMeshlets(CommandList& cmd, const MeshShaderPush& push) {
  cmd.Push(push);
  // One task workgroup (32 threads) per 32 meshlets; the task stage culls and
  // compacts, then dispatches the surviving mesh workgroups.
  cmd.DrawMeshTasks((push.meshlet_count + 31u) / 32u, 1, 1);
}

}  // namespace rec::render

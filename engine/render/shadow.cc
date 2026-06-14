#include "render/shadow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include "asset/mesh.h"
#include "core/log.h"
#include "render/shader_util.h"
#include "shaders/shadow_ps_hlsl.h"
#include "shaders/shadow_skin_vs_hlsl.h"
#include "shaders/shadow_vs_hlsl.h"

namespace rec::render {
namespace {

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Mul(const Vec3& v, f32 s) { return {v.x * s, v.y * s, v.z * s}; }

}  // namespace

bool ShadowPass::Initialize(Device& device, VkDescriptorSetLayout material_layout) {
  // Covers the skinned permutation's trailing bone_address + skin_offset; the
  // static caster only writes the two leading matrices.
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(Mat4) + 16};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &material_layout;  // set 0: alpha-test inputs
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule vs = CreateShaderModule(device.device(), k_shadow_vs_hlsl, sizeof(k_shadow_vs_hlsl));
  VkShaderModule skin_vs =
      CreateShaderModule(device.device(), k_shadow_skin_vs_hlsl, sizeof(k_shadow_skin_vs_hlsl));
  VkShaderModule ps = CreateShaderModule(device.device(), k_shadow_ps_hlsl, sizeof(k_shadow_ps_hlsl));
  if (vs == VK_NULL_HANDLE || skin_vs == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) return false;

  // Shared state for both permutations.
  VkPipelineInputAssemblyStateCreateInfo ia{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;  // thin geometry must cast from both sides
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  raster.depthBiasEnable = VK_TRUE;  // slope-scaled, kills most shadow acne
  raster.depthBiasConstantFactor = 1.25f;
  raster.depthBiasSlopeFactor = 2.0f;

  VkPipelineMultisampleStateCreateInfo ms{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_TRUE;
  ds.depthCompareOp = VK_COMPARE_OP_LESS;  // standard depth, nearest occluder wins

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;

  VkPipelineColorBlendStateCreateInfo blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 0;

  VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

  // Position (binding 0) + uv for alpha test; the skinned variant adds the bone
  // index/weight stream (binding 1) so it skins in the vertex stage.
  VkVertexInputBindingDescription bindings[2] = {};
  bindings[0].binding = 0;
  bindings[0].stride = sizeof(asset::Vertex);
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  bindings[1].binding = 1;
  bindings[1].stride = sizeof(asset::SkinnedVertexExtra);
  bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[4] = {
      {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, position)},
      {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(asset::Vertex, uv)},
      {.location = 5, .binding = 1, .format = VK_FORMAT_R8G8B8A8_UINT,
       .offset = offsetof(asset::SkinnedVertexExtra, bone_indices)},
      {.location = 6, .binding = 1, .format = VK_FORMAT_R8G8B8A8_UNORM,
       .offset = offsetof(asset::SkinnedVertexExtra, bone_weights)}};

  auto make_pipeline = [&](VkShaderModule vertex, bool skinned, VkPipeline* out) -> bool {
    VkPipelineShaderStageCreateInfo stages[2];
    stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = skinned ? 2u : 1u;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = skinned ? 4u : 2u;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    return vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, out) ==
           VK_SUCCESS;
  };

  bool ok = make_pipeline(vs, false, &pipeline_) && make_pipeline(skin_vs, true, &skinned_pipeline_);
  vkDestroyShaderModule(device.device(), vs, nullptr);
  vkDestroyShaderModule(device.device(), skin_vs, nullptr);
  vkDestroyShaderModule(device.device(), ps, nullptr);
  if (!ok) {
    REC_ERROR("shadow pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    cascades_[i] = device.CreateBuffer(sizeof(CascadeData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    if (!cascades_[i].mapped) return false;
    std::memset(cascades_[i].mapped, 0, sizeof(CascadeData));
  }
  return true;
}

void ShadowPass::Configure(const Settings& settings) {
  settings_ = settings;
  settings_.cascade_count = std::clamp(settings_.cascade_count, 1u, kMaxCascades);
}

void ShadowPass::Update(const Vec3& eye, const Vec3& forward, const Vec3& right, const Vec3& up,
                        f32 fov_y, f32 aspect, const Vec3& sun_direction, u32 frame_slot) {
  const u32 count = settings_.cascade_count;
  const f32 near_plane = 0.1f;
  const f32 far_plane = settings_.distance;
  const f32 tan_half = std::tan(fov_y * 0.5f);
  const f32 lambda = 0.7f;       // log/uniform split blend
  const f32 back_pad = 80.0f;    // caster range behind the slice, toward the sun

  f32 splits[kMaxCascades + 1];
  splits[0] = near_plane;
  for (u32 i = 1; i <= count; ++i) {
    f32 p = static_cast<f32>(i) / static_cast<f32>(count);
    f32 log_split = near_plane * std::pow(far_plane / near_plane, p);
    f32 uniform_split = near_plane + (far_plane - near_plane) * p;
    splits[i] = lambda * log_split + (1.0f - lambda) * uniform_split;
  }

  Vec3 light_dir = Normalize(sun_direction);  // travel direction = look direction
  Vec3 up_ref = std::abs(light_dir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};

  current_ = CascadeData{};
  for (u32 i = 0; i < count; ++i) {
    f32 cn = splits[i], cf = splits[i + 1];
    Vec3 corners[8];
    u32 c = 0;
    for (f32 d : {cn, cf}) {
      f32 hh = d * tan_half;
      f32 hw = hh * aspect;
      Vec3 center_d = Add(eye, Mul(forward, d));
      for (f32 sx : {-1.0f, 1.0f})
        for (f32 sy : {-1.0f, 1.0f})
          corners[c++] = Add(center_d, Add(Mul(right, hw * sx), Mul(up, hh * sy)));
    }

    Vec3 center{0, 0, 0};
    for (const Vec3& p : corners) center = Add(center, Mul(p, 1.0f / 8.0f));
    f32 radius = 0.0f;
    for (const Vec3& p : corners) {
      Vec3 v = {p.x - center.x, p.y - center.y, p.z - center.z};
      radius = std::max(radius, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;  // quantize so it stops pulsing

    Vec3 light_eye = {center.x - light_dir.x * (radius + back_pad),
                      center.y - light_dir.y * (radius + back_pad),
                      center.z - light_dir.z * (radius + back_pad)};
    Mat4 light_view = LookAt(light_eye, center, up_ref);
    Mat4 light_proj =
        Orthographic(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + back_pad);
    Mat4 light_vp = light_proj * light_view;

    // Texel snap: round the projected world origin to whole shadow texels so the
    // cascade slides in texel steps and stops shimmering as the camera moves.
    Vec3 origin_ndc = TransformPoint(light_vp, Vec3{0, 0, 0});
    f32 half_res = settings_.resolution * 0.5f;
    f32 sx = origin_ndc.x * half_res;
    f32 sy = origin_ndc.y * half_res;
    f32 dx = (std::round(sx) - sx) / half_res;
    f32 dy = (std::round(sy) - sy) / half_res;
    light_vp.m[12] += dx;
    light_vp.m[13] += dy;

    current_.light_view_proj[i] = light_vp;
  }

  current_.p0[0] = static_cast<f32>(count);
  current_.p0[1] = settings_.depth_bias;
  current_.p0[2] = 1.0f / static_cast<f32>(count);
  current_.p0[3] = 1.5f / static_cast<f32>(settings_.resolution);  // inset, a few texels
  current_.p1[0] = 1.0f / static_cast<f32>(settings_.resolution);  // cascade-local texel
  current_.p1[1] = 0.0f;
  current_.p1[2] = settings_.normal_bias;
  current_.p1[3] = 0.0f;
  std::memcpy(cascades_[frame_slot].mapped, &current_, sizeof(CascadeData));
}

void ShadowPass::Render(VkCommandBuffer cmd, VkImageView atlas_view,
                        const std::function<void(VkCommandBuffer, VkPipelineLayout)>& draw) {
  const u32 res = settings_.resolution;

  // The graph already put the atlas in DEPTH_ATTACHMENT layout for this write.
  VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = atlas_view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depthStencil = {1.0f, 0};
  VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {res * settings_.cascade_count, res}};
  rendering.layerCount = 1;
  rendering.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rendering);
  // The draw callback binds pipeline()/skinned_pipeline() per mesh; both share
  // this layout, so the per-cascade light matrix push below stays valid.

  for (u32 i = 0; i < settings_.cascade_count; ++i) {
    VkViewport vp{static_cast<f32>(i * res), 0.0f, static_cast<f32>(res), static_cast<f32>(res),
                  0.0f, 1.0f};
    VkRect2D scissor{{static_cast<i32>(i * res), 0}, {res, res}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                       &current_.light_view_proj[i]);
    draw(cmd, layout_);
  }
  vkCmdEndRendering(cmd);
}

void ShadowPass::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (skinned_pipeline_) vkDestroyPipeline(device.device(), skinned_pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(cascades_[i]);
  pipeline_ = VK_NULL_HANDLE;
  skinned_pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
}

}  // namespace rec::render

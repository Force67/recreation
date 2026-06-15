#include "render/meshlet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/shader_util.h"
#include "shaders/meshlet_ms_hlsl.h"
#include "shaders/meshlet_ps_hlsl.h"

namespace rec::render {
namespace {

constexpr u32 kMaxVerts = 64;
constexpr u32 kMaxTris = 124;  // cone-bounded growth usually finalizes a cluster first

struct MeshletPush {
  Mat4 view_proj;
  f32 planes[5][4];
  f32 camera[4];
};

struct Built {
  std::vector<MeshletPass::Meshlet> meshlets;
  std::vector<u32> vertex_indices;  // index into vertices
  std::vector<u32> triangles;       // 3 local indices packed per u32
  std::vector<MeshletPass::Vertex> vertices;
};

u32 Part1By2(u32 x) {
  x &= 0x3ff;
  x = (x | (x << 16)) & 0x30000ff;
  x = (x | (x << 8)) & 0x300f00f;
  x = (x | (x << 4)) & 0x30c30c3;
  x = (x | (x << 2)) & 0x9249249;
  return x;
}
u32 Morton3(u32 x, u32 y, u32 z) { return Part1By2(x) | (Part1By2(y) << 1) | (Part1By2(z) << 2); }

// Greedy clustering: append triangles to the current meshlet until it would
// exceed the vertex or triangle budget, then start a new one. Per meshlet a
// bounding sphere and a backface normal cone are computed for cluster culling.
Built BuildMeshlets(const asset::MeshLod& lod) {
  Built out;
  out.vertices.reserve(lod.vertices.size());
  for (const asset::Vertex& v : lod.vertices) {
    out.vertices.push_back({v.position[0], v.position[1], v.position[2], v.normal[0], v.normal[1],
                            v.normal[2]});
  }

  u32 local_global[kMaxVerts];  // local index -> global vertex index
  u8 local_count = 0;
  Vec3 cone_sum{0, 0, 0};  // running sum of unit triangle normals, to bound the cone
  u32 cur_vert_offset = 0, cur_tri_offset = 0;

  auto finalize = [&]() {
    if (local_count == 0) return;
    MeshletPass::Meshlet m{};
    m.vertex_offset = cur_vert_offset;
    m.triangle_offset = cur_tri_offset;
    m.vertex_count = local_count;
    m.triangle_count = static_cast<u32>(out.triangles.size()) - cur_tri_offset;

    // Bounding sphere from the meshlet's unique vertices.
    Vec3 center{0, 0, 0};
    for (u32 i = 0; i < local_count; ++i) {
      const MeshletPass::Vertex& v = out.vertices[local_global[i]];
      center = center + Vec3{v.px, v.py, v.pz};
    }
    center = center * (1.0f / static_cast<f32>(local_count));
    f32 radius = 0.0f;
    for (u32 i = 0; i < local_count; ++i) {
      const MeshletPass::Vertex& v = out.vertices[local_global[i]];
      Vec3 d = Vec3{v.px, v.py, v.pz} - center;
      radius = std::max(radius, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
    }
    m.center_radius[0] = center.x;
    m.center_radius[1] = center.y;
    m.center_radius[2] = center.z;
    m.center_radius[3] = radius;

    // Normal cone: average triangle normal as the axis, the widest deviation as
    // the cutoff. A cone wider than a hemisphere can never be wholly backfacing.
    Vec3 axis{0, 0, 0};
    for (u32 t = m.triangle_offset; t < m.triangle_offset + m.triangle_count; ++t) {
      u32 packed = out.triangles[t];
      const MeshletPass::Vertex& a = out.vertices[local_global[packed & 0xff]];
      const MeshletPass::Vertex& b = out.vertices[local_global[(packed >> 8) & 0xff]];
      const MeshletPass::Vertex& c = out.vertices[local_global[(packed >> 16) & 0xff]];
      Vec3 e1 = Vec3{b.px, b.py, b.pz} - Vec3{a.px, a.py, a.pz};
      Vec3 e2 = Vec3{c.px, c.py, c.pz} - Vec3{a.px, a.py, a.pz};
      Vec3 n = Cross(e1, e2);
      f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
      if (len > 1e-8f) axis = axis + n * (1.0f / len);
    }
    f32 alen = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (alen > 1e-6f) {
      axis = axis * (1.0f / alen);
      f32 min_c = 1.0f;
      for (u32 t = m.triangle_offset; t < m.triangle_offset + m.triangle_count; ++t) {
        u32 packed = out.triangles[t];
        const MeshletPass::Vertex& a = out.vertices[local_global[packed & 0xff]];
        const MeshletPass::Vertex& b = out.vertices[local_global[(packed >> 8) & 0xff]];
        const MeshletPass::Vertex& c = out.vertices[local_global[(packed >> 16) & 0xff]];
        Vec3 e1 = Vec3{b.px, b.py, b.pz} - Vec3{a.px, a.py, a.pz};
        Vec3 e2 = Vec3{c.px, c.py, c.pz} - Vec3{a.px, a.py, a.pz};
        Vec3 n = Cross(e1, e2);
        f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-8f) min_c = std::min(min_c, (n.x * axis.x + n.y * axis.y + n.z * axis.z) / len);
      }
      m.cone[0] = axis.x;
      m.cone[1] = axis.y;
      m.cone[2] = axis.z;
      m.cone[3] = min_c > 0.0f ? std::sqrt(1.0f - min_c * min_c) : 2.0f;  // 2 = never cull
    } else {
      m.cone[3] = 2.0f;  // degenerate, never cone-cull
    }

    out.meshlets.push_back(m);
    cur_vert_offset = static_cast<u32>(out.vertex_indices.size());
    cur_tri_offset = static_cast<u32>(out.triangles.size());
    local_count = 0;
    cone_sum = {0, 0, 0};
  };

  // Order triangles along a Morton curve over their centroids so the greedy
  // clusters become compact spatial patches (tight normal cones -> effective
  // backface cone culling), not index-order bands that wrap the whole surface.
  const size_t tri_total = lod.indices.size() / 3;
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const asset::Vertex& v : lod.vertices) {
    lo = {std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2])};
    hi = {std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2])};
  }
  Vec3 ext{std::max(hi.x - lo.x, 1e-6f), std::max(hi.y - lo.y, 1e-6f), std::max(hi.z - lo.z, 1e-6f)};
  std::vector<std::pair<u32, u32>> order(tri_total);  // (morton, triangle index)
  for (size_t t = 0; t < tri_total; ++t) {
    Vec3 c{0, 0, 0};
    for (u32 k = 0; k < 3; ++k) {
      const asset::Vertex& v = lod.vertices[lod.indices[t * 3 + k]];
      c = c + Vec3{v.position[0], v.position[1], v.position[2]};
    }
    c = c * (1.0f / 3.0f);
    u32 qx = static_cast<u32>(std::clamp((c.x - lo.x) / ext.x, 0.0f, 1.0f) * 1023.0f);
    u32 qy = static_cast<u32>(std::clamp((c.y - lo.y) / ext.y, 0.0f, 1.0f) * 1023.0f);
    u32 qz = static_cast<u32>(std::clamp((c.z - lo.z) / ext.z, 0.0f, 1.0f) * 1023.0f);
    order[t] = {Morton3(qx, qy, qz), static_cast<u32>(t)};
  }
  std::sort(order.begin(), order.end());

  for (const auto& ord : order) {
    size_t i = static_cast<size_t>(ord.second) * 3;
    u32 g[3] = {lod.indices[i], lod.indices[i + 1], lod.indices[i + 2]};
    u8 local[3];
    // Count how many of this triangle's vertices are not already in the meshlet.
    u32 added = 0;
    for (u32 k = 0; k < 3; ++k) {
      bool found = false;
      for (u32 l = 0; l < local_count; ++l) {
        if (local_global[l] == g[k]) {
          found = true;
          break;
        }
      }
      if (!found) ++added;
    }
    // This triangle's unit normal, to keep the meshlet's normal cone tight.
    const asset::Vertex& va = lod.vertices[g[0]];
    const asset::Vertex& vb = lod.vertices[g[1]];
    const asset::Vertex& vc = lod.vertices[g[2]];
    Vec3 pa{va.position[0], va.position[1], va.position[2]};
    Vec3 n = Cross(Vec3{vb.position[0], vb.position[1], vb.position[2]} - pa,
                   Vec3{vc.position[0], vc.position[1], vc.position[2]} - pa);
    f32 nlen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nlen > 1e-8f) n = n * (1.0f / nlen);
    // Bound the cone half-angle (~45deg from the running mean) so backface cone
    // culling stays effective; finalize early when a triangle would widen it.
    bool cone_break = false;
    if (local_count > 0) {
      f32 slen = std::sqrt(cone_sum.x * cone_sum.x + cone_sum.y * cone_sum.y + cone_sum.z * cone_sum.z);
      if (slen > 1e-6f && (n.x * cone_sum.x + n.y * cone_sum.y + n.z * cone_sum.z) / slen < 0.85f) {
        cone_break = true;
      }
    }
    bool tri_full = out.triangles.size() - cur_tri_offset >= kMaxTris;
    if (local_count + added > kMaxVerts || tri_full || cone_break) finalize();
    if (nlen > 1e-8f) cone_sum = cone_sum + n;

    for (u32 k = 0; k < 3; ++k) {
      u8 idx = 0xff;
      for (u32 l = 0; l < local_count; ++l) {
        if (local_global[l] == g[k]) {
          idx = static_cast<u8>(l);
          break;
        }
      }
      if (idx == 0xff) {
        idx = local_count;
        local_global[local_count++] = g[k];
        out.vertex_indices.push_back(g[k]);
      }
      local[k] = idx;
    }
    out.triangles.push_back(local[0] | (local[1] << 8) | (local[2] << 16));
  }
  finalize();
  return out;
}

ByteSpan Span(const void* data, size_t bytes) {
  return ByteSpan(static_cast<const u8*>(data), bytes);
}

}  // namespace

bool MeshletPass::Initialize(Device& device, VkFormat color_format, VkFormat depth_format) {
  available_ = device.caps().mesh_shaders;
  if (!available_) return true;  // no mesh-shader support: pass stays inert, demo skips it

  VkDescriptorSetLayoutBinding bindings[5]{};
  for (u32 i = 0; i < 5; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
  }
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 5;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(MeshletPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule ms = CreateShaderModule(device.device(), k_meshlet_ms_hlsl, sizeof(k_meshlet_ms_hlsl));
  VkShaderModule ps = CreateShaderModule(device.device(), k_meshlet_ps_hlsl, sizeof(k_meshlet_ps_hlsl));
  if (ms == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) return false;
  VkPipelineShaderStageCreateInfo stages[2];
  stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
  stages[0].module = ms;
  stages[0].pName = "main";
  stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps;
  stages[1].pName = "main";

  VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_BACK_BIT;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_TRUE;
  ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed z
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend_state.attachmentCount = 1;
  blend_state.pAttachments = &blend;
  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;
  VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format;
  rendering.depthAttachmentFormat = depth_format;

  VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &ms_state;
  info.pDepthStencilState = &ds;
  info.pColorBlendState = &blend_state;
  info.pDynamicState = &dynamic;
  info.layout = layout_;
  VkResult r =
      vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device.device(), ms, nullptr);
  vkDestroyShaderModule(device.device(), ps, nullptr);
  if (r != VK_SUCCESS) {
    REC_ERROR("meshlet pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    counters_[i] = device.CreateBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
  }
  return true;
}

void MeshletPass::Upload(Device& device, const asset::Mesh& mesh) {
  if (!available_ || mesh.lods.empty()) return;
  device.DestroyBuffer(meshlets_);
  device.DestroyBuffer(meshlet_vertices_);
  device.DestroyBuffer(meshlet_triangles_);
  device.DestroyBuffer(vertices_);

  Built built = BuildMeshlets(mesh.lods[0]);
  meshlet_count_ = static_cast<u32>(built.meshlets.size());
  if (meshlet_count_ == 0) return;

  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  meshlets_ = device.CreateBufferWithData(
      Span(built.meshlets.data(), built.meshlets.size() * sizeof(Meshlet)), storage);
  meshlet_vertices_ = device.CreateBufferWithData(
      Span(built.vertex_indices.data(), built.vertex_indices.size() * sizeof(u32)), storage);
  meshlet_triangles_ = device.CreateBufferWithData(
      Span(built.triangles.data(), built.triangles.size() * sizeof(u32)), storage);
  vertices_ = device.CreateBufferWithData(
      Span(built.vertices.data(), built.vertices.size() * sizeof(Vertex)), storage);
  REC_INFO("meshlet: {} meshlets from {} tris ({} verts)", meshlet_count_,
           mesh.lods[0].indices.size() / 3, mesh.lods[0].vertices.size());
}

u32 MeshletPass::last_visible(u32 slot) const {
  return counters_[slot % kFramesInFlight].mapped
             ? *static_cast<const u32*>(counters_[slot % kFramesInFlight].mapped)
             : 0;
}

void MeshletPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                             const Mat4& view_proj, const f32 planes[5][4], const Vec3& camera,
                             u32 slot) {
  if (!active()) return;
  slot %= kFramesInFlight;
  if (counters_[slot].mapped) *static_cast<u32*>(counters_[slot].mapped) = 0;

  MeshletPush push{};
  push.view_proj = view_proj;
  std::memcpy(push.planes, planes, sizeof(push.planes));
  push.camera[0] = camera.x;
  push.camera[1] = camera.y;
  push.camera[2] = camera.z;
  VkBuffer counter = counters_[slot].buffer;

  graph.AddPass(
      "meshlet",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, color, depth, push, counter](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);
        VkBuffer buffers[5] = {meshlets_.buffer, meshlet_vertices_.buffer, meshlet_triangles_.buffer,
                               vertices_.buffer, counter};
        VkDescriptorBufferInfo infos[5];
        VkWriteDescriptorSet writes[5];
        for (u32 i = 0; i < 5; ++i) {
          infos[i] = {buffers[i], 0, VK_WHOLE_SIZE};
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 5, writes, 0, nullptr);

        const GpuImage& target = ctx.graph->image(color);
        VkRenderingAttachmentInfo color_att{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color_att.imageView = target.view;
        color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth_att{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_att.imageView = ctx.graph->image(depth).view;
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, target.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color_att;
        rendering.pDepthAttachment = &depth_att;
        vkCmdBeginRendering(ctx.cmd, &rendering);
        VkViewport vp{0, 0, static_cast<f32>(target.extent.width),
                      static_cast<f32>(target.extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, target.extent};
        vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(push), &push);
        vkCmdDrawMeshTasksEXT(ctx.cmd, meshlet_count_, 1, 1);
        vkCmdEndRendering(ctx.cmd);
      });
}

void MeshletPass::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  device.DestroyBuffer(meshlets_);
  device.DestroyBuffer(meshlet_vertices_);
  device.DestroyBuffer(meshlet_triangles_);
  device.DestroyBuffer(vertices_);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(counters_[i]);
}

}  // namespace rec::render

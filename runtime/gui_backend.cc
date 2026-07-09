#include "gui_backend.h"

#include <algorithm>
#include <cstring>

#include <ugui/render/vertex.h>

#include "render/util/shader_util.h"
#include "shaders/ugui_frost_ps_hlsl.h"
#include "shaders/ugui_frost_vs_hlsl.h"
#include "shaders/ugui_quad_ps_hlsl.h"
#include "shaders/ugui_quad_vs_hlsl.h"
#include "shaders/ugui_text_ps_hlsl.h"
#include "shaders/ugui_text_vs_hlsl.h"

namespace rx::ui {
namespace {

void RhiFormatToVk(ugui::RHIFormat f, VkFormat& fmt, uint32_t& pixel_size) {
  switch (f) {
    case ugui::RHIFormat::kR8Unorm:
      fmt = VK_FORMAT_R8_UNORM;
      pixel_size = 1;
      break;
    case ugui::RHIFormat::kBgra8Unorm:
      fmt = VK_FORMAT_B8G8R8A8_UNORM;
      pixel_size = 4;
      break;
    case ugui::RHIFormat::kRgba32Float:
      fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
      pixel_size = 16;
      break;
    case ugui::RHIFormat::kRgba8Unorm:
    default:
      fmt = VK_FORMAT_R8G8B8A8_UNORM;
      pixel_size = 4;
      break;
  }
}

}  // namespace

uint32_t GuiRenderBackend::FindMemoryType(uint32_t type_filter,
                                          VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(info_.physical_device, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  return 0;
}

void GuiRenderBackend::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags props, GpuBuffer& out) {
  VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vkCreateBuffer(info_.device, &ci, nullptr, &out.buffer);
  VkMemoryRequirements reqs;
  vkGetBufferMemoryRequirements(info_.device, out.buffer, &reqs);
  VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = reqs.size;
  ai.memoryTypeIndex = FindMemoryType(reqs.memoryTypeBits, props);
  vkAllocateMemory(info_.device, &ai, nullptr, &out.memory);
  vkBindBufferMemory(info_.device, out.buffer, out.memory, 0);
  out.capacity = size;
}

void GuiRenderBackend::DestroyBuffer(GpuBuffer& b) {
  if (b.buffer) vkDestroyBuffer(info_.device, b.buffer, nullptr);
  if (b.memory) vkFreeMemory(info_.device, b.memory, nullptr);
  b = {};
}

void GuiRenderBackend::UploadBuffer(GpuBuffer& b, VkBufferUsageFlags usage, const void* src,
                                    VkDeviceSize bytes) {
  if (bytes == 0) return;
  if (b.capacity < bytes) {
    DestroyBuffer(b);
    VkDeviceSize cap = std::max<VkDeviceSize>(bytes, 4096);
    CreateBuffer(cap, usage,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, b);
  }
  void* dst = nullptr;
  vkMapMemory(info_.device, b.memory, 0, bytes, 0, &dst);
  std::memcpy(dst, src, static_cast<size_t>(bytes));
  vkUnmapMemory(info_.device, b.memory);
}

VkPipeline GuiRenderBackend::CreatePipeline(const unsigned char* vs, size_t vs_size,
                                            const unsigned char* fs, size_t fs_size,
                                            uint32_t attr_count) {
  VkShaderModule vert = rx::render::CreateShaderModule(info_.device, vs, vs_size);
  VkShaderModule frag = rx::render::CreateShaderModule(info_.device, fs, fs_size);
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(ugui::Vertex2D);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  VkVertexInputAttributeDescription attrs[9] = {};
  attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ugui::Vertex2D, pos)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ugui::Vertex2D, uv)};
  attrs[2] = {2, 0, VK_FORMAT_R32_UINT, offsetof(ugui::Vertex2D, color)};
  attrs[3] = {3, 0, VK_FORMAT_R32_UINT, offsetof(ugui::Vertex2D, color2)};
  attrs[4] = {4, 0, VK_FORMAT_R32_UINT, offsetof(ugui::Vertex2D, corner_radii)};
  attrs[5] = {5, 0, VK_FORMAT_R32_SFLOAT, offsetof(ugui::Vertex2D, softness)};
  attrs[6] = {6, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ugui::Vertex2D, half_size)};
  attrs[7] = {7, 0, VK_FORMAT_R32_SFLOAT, offsetof(ugui::Vertex2D, border_width)};
  attrs[8] = {8, 0, VK_FORMAT_R32_UINT, offsetof(ugui::Vertex2D, border_color)};

  VkPipelineVertexInputStateCreateInfo vin{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vin.vertexBindingDescriptionCount = 1;
  vin.pVertexBindingDescriptions = &binding;
  // The text pipeline's shader only reads pos/uv/color (the first 3); declaring
  // only what each shader consumes keeps validation quiet.
  vin.vertexAttributeDescriptionCount = attr_count;
  vin.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

  VkPipelineColorBlendAttachmentState ba{};
  ba.blendEnable = VK_TRUE;
  ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  ba.colorBlendOp = VK_BLEND_OP_ADD;
  ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  ba.alphaBlendOp = VK_BLEND_OP_ADD;
  ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo cb{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;

  VkPipelineRenderingCreateInfo rendering{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &info_.color_format;

  VkGraphicsPipelineCreateInfo pi{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.pNext = &rendering;
  pi.stageCount = 2;
  pi.pStages = stages;
  pi.pVertexInputState = &vin;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pDepthStencilState = &depth;
  pi.pColorBlendState = &cb;
  pi.pDynamicState = &ds;
  pi.layout = pipeline_layout_;

  VkPipeline pipeline = VK_NULL_HANDLE;
  vkCreateGraphicsPipelines(info_.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
  vkDestroyShaderModule(info_.device, vert, nullptr);
  vkDestroyShaderModule(info_.device, frag, nullptr);
  return pipeline;
}

VkSampler GuiRenderBackend::MakeSampler(VkFilter filter) {
  VkSamplerCreateInfo s{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  s.magFilter = filter;
  s.minFilter = filter;
  s.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  s.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler out = VK_NULL_HANDLE;
  vkCreateSampler(info_.device, &s, nullptr, &out);
  return out;
}

GuiRenderBackend::Texture GuiRenderBackend::MakeTexture(uint32_t w, uint32_t h, VkFormat fmt,
                                                        uint32_t pixel_size, const void* pixels,
                                                        VkSampler sampler) {
  Texture t;
  VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * pixel_size;

  GpuBuffer staging;
  CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging);
  void* data = nullptr;
  vkMapMemory(info_.device, staging.memory, 0, size, 0, &data);
  std::memcpy(data, pixels, static_cast<size_t>(size));
  vkUnmapMemory(info_.device, staging.memory);

  VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.format = fmt;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  vkCreateImage(info_.device, &ici, nullptr, &t.image);

  VkMemoryRequirements reqs;
  vkGetImageMemoryRequirements(info_.device, t.image, &reqs);
  VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = reqs.size;
  ai.memoryTypeIndex = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(info_.device, &ai, nullptr, &t.memory);
  vkBindImageMemory(info_.device, t.image, t.memory, 0);

  // One-time upload + layout transitions on a transient command buffer.
  VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = info_.queue_family;
  pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  VkCommandPool pool;
  vkCreateCommandPool(info_.device, &pci, nullptr, &pool);
  VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(info_.device, &cai, &cmd);
  VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = t.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &region);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(info_.queue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(info_.queue);
  vkDestroyCommandPool(info_.device, pool, nullptr);
  DestroyBuffer(staging);

  VkImageViewCreateInfo vci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = t.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = fmt;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(info_.device, &vci, nullptr, &t.view);

  VkDescriptorSetAllocateInfo dai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descriptor_pool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &set_layout_;
  vkAllocateDescriptorSets(info_.device, &dai, &t.set);

  VkDescriptorImageInfo dii{};
  dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dii.imageView = t.view;
  dii.sampler = sampler;
  VkWriteDescriptorSet wd{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wd.dstSet = t.set;
  wd.dstBinding = 0;
  wd.descriptorCount = 1;
  wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wd.pImageInfo = &dii;
  vkUpdateDescriptorSets(info_.device, 1, &wd, 0, nullptr);
  return t;
}

void GuiRenderBackend::FreeTexture(Texture& t) {
  if (t.set) vkFreeDescriptorSets(info_.device, descriptor_pool_, 1, &t.set);
  if (t.view) vkDestroyImageView(info_.device, t.view, nullptr);
  if (t.image) vkDestroyImage(info_.device, t.image, nullptr);
  if (t.memory) vkFreeMemory(info_.device, t.memory, nullptr);
  t = {};
}

bool GuiRenderBackend::Init(const InitInfo& info) {
  info_ = info;
  if (info_.frames_in_flight < 1) info_.frames_in_flight = 2;

  VkDescriptorSetLayoutBinding b{};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo lci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 1;
  lci.pBindings = &b;
  if (vkCreateDescriptorSetLayout(info_.device, &lci, nullptr, &set_layout_) != VK_SUCCESS)
    return false;

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pcr.offset = 0;
  pcr.size = sizeof(float) * 4;
  VkPipelineLayoutCreateInfo plci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(info_.device, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS)
    return false;

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128};
  VkDescriptorPoolCreateInfo dpci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &ps;
  dpci.maxSets = 128;
  if (vkCreateDescriptorPool(info_.device, &dpci, nullptr, &descriptor_pool_) != VK_SUCCESS)
    return false;

  linear_sampler_ = MakeSampler(VK_FILTER_LINEAR);
  nearest_sampler_ = MakeSampler(VK_FILTER_NEAREST);

  quad_pipeline_ = CreatePipeline(k_ugui_quad_vs_hlsl, sizeof(k_ugui_quad_vs_hlsl),
                                  k_ugui_quad_ps_hlsl, sizeof(k_ugui_quad_ps_hlsl), 9);
  text_pipeline_ = CreatePipeline(k_ugui_text_vs_hlsl, sizeof(k_ugui_text_vs_hlsl),
                                  k_ugui_text_ps_hlsl, sizeof(k_ugui_text_ps_hlsl), 3);
  // The frosted-glass pipeline shares the quad vertex layout and pipeline layout
  // (one combined image sampler = the blurred backdrop; vertex push constant).
  frost_pipeline_ = CreatePipeline(k_ugui_frost_vs_hlsl, sizeof(k_ugui_frost_vs_hlsl),
                                   k_ugui_frost_ps_hlsl, sizeof(k_ugui_frost_ps_hlsl), 9);
  if (!quad_pipeline_ || !text_pipeline_ || !frost_pipeline_) return false;

  uint32_t white = 0xFFFFFFFFu;
  white_ = MakeTexture(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, &white, linear_sampler_);

  // A frost descriptor set per frame in flight, rebound to the backdrop each
  // frame in Render (so updating one never races a submission still reading it).
  frost_sets_.resize(info_.frames_in_flight, VK_NULL_HANDLE);
  for (auto& set : frost_sets_) {
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_;
    vkAllocateDescriptorSets(info_.device, &ai, &set);
  }

  frames_.resize(info_.frames_in_flight);
  return true;
}

void GuiRenderBackend::Shutdown() {
  if (!info_.device) return;
  vkDeviceWaitIdle(info_.device);
  for (auto& f : frames_) {
    DestroyBuffer(f.quad_vtx);
    DestroyBuffer(f.quad_idx);
    DestroyBuffer(f.text_vtx);
    DestroyBuffer(f.text_idx);
  }
  frames_.clear();
  for (auto& kv : user_textures_) FreeTexture(kv.second.tex);
  user_textures_.clear();
  FreeTexture(font_);
  FreeTexture(white_);
  if (quad_pipeline_) vkDestroyPipeline(info_.device, quad_pipeline_, nullptr);
  if (text_pipeline_) vkDestroyPipeline(info_.device, text_pipeline_, nullptr);
  if (frost_pipeline_) vkDestroyPipeline(info_.device, frost_pipeline_, nullptr);
  if (pipeline_layout_) vkDestroyPipelineLayout(info_.device, pipeline_layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(info_.device, set_layout_, nullptr);
  if (descriptor_pool_) vkDestroyDescriptorPool(info_.device, descriptor_pool_, nullptr);
  if (linear_sampler_) vkDestroySampler(info_.device, linear_sampler_, nullptr);
  if (nearest_sampler_) vkDestroySampler(info_.device, nearest_sampler_, nullptr);
  info_ = {};
}

void GuiRenderBackend::NewFrame() {
  frame_index_ = (frame_index_ + 1) % info_.frames_in_flight;
}

void GuiRenderBackend::SetBackdrop(VkImageView view, VkSampler sampler) {
  backdrop_view_ = view;
  backdrop_sampler_ = sampler;
}

bool GuiRenderBackend::UpdateFontAtlas(const uint8_t* pixels, uint32_t width, uint32_t height) {
  if (!pixels || width == 0 || height == 0) return false;
  vkDeviceWaitIdle(info_.device);
  FreeTexture(font_);
  font_ = MakeTexture(width, height, VK_FORMAT_R8_UNORM, 1, pixels, nearest_sampler_);
  return font_.image != VK_NULL_HANDLE;
}

void GuiRenderBackend::Render(const ugui::DrawData& dd, VkCommandBuffer cmd) {
  if (!dd.valid || dd.command_count == 0) return;
  if (dd.display_size.x <= 0.0f || dd.display_size.y <= 0.0f) return;

  FrameBuffers& fb = frames_[frame_index_];
  UploadBuffer(fb.quad_vtx, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, dd.quad_vertices,
               dd.quad_vertex_count * sizeof(ugui::Vertex2D));
  UploadBuffer(fb.quad_idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, dd.quad_indices,
               dd.quad_index_count * sizeof(uint32_t));
  UploadBuffer(fb.text_vtx, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, dd.text_vertices,
               dd.text_vertex_count * sizeof(ugui::Vertex2D));
  UploadBuffer(fb.text_idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, dd.text_indices,
               dd.text_index_count * sizeof(uint32_t));

  float sx = dd.framebuffer_scale.x > 0 ? dd.framebuffer_scale.x : 1.0f;
  float sy = dd.framebuffer_scale.y > 0 ? dd.framebuffer_scale.y : 1.0f;

  VkViewport vp{};
  vp.x = 0;
  vp.y = 0;
  vp.width = dd.display_size.x * sx;
  vp.height = dd.display_size.y * sy;
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &vp);

  float push[4] = {2.0f / dd.display_size.x, 2.0f / dd.display_size.y, -1.0f, -1.0f};
  vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);

  // Backdrop blur: bind this frame's frost set to the renderer-supplied blurred
  // copy of what is behind the UI. Absent (or no blur command) -> plain quads.
  const bool have_backdrop = backdrop_view_ != VK_NULL_HANDLE && !frost_sets_.empty() &&
                             frost_sets_[frame_index_] != VK_NULL_HANDLE;
  if (have_backdrop) {
    VkDescriptorImageInfo ii{backdrop_sampler_, backdrop_view_,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = frost_sets_[frame_index_];
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(info_.device, 1, &w, 0, nullptr);
  }

  VkPipeline bound = VK_NULL_HANDLE;
  for (uint32_t i = 0; i < dd.command_count; ++i) {
    const ugui::DrawCmd& c = dd.commands[i];
    if (c.elem_count == 0) continue;

    const bool frost = c.blur > 0.0f && have_backdrop && !c.is_text;
    VkPipeline want = c.is_text ? text_pipeline_ : (frost ? frost_pipeline_ : quad_pipeline_);
    if (want != bound) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
      bound = want;
    }

    VkDescriptorSet set;
    if (frost) {
      set = frost_sets_[frame_index_];
    } else if (c.is_text || c.texture_id == ugui::kFontTextureId) {
      set = font_.set;
    } else if (c.texture_id == ugui::kNullTextureId) {
      set = white_.set;
    } else {
      auto it = user_textures_.find(c.texture_id);
      set = it != user_textures_.end() ? it->second.tex.set : white_.set;
    }
    if (set == VK_NULL_HANDLE) set = white_.set;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0,
                            nullptr);

    VkRect2D scissor{};
    float x0 = std::max(0.0f, c.clip_rect.x) * sx;
    float y0 = std::max(0.0f, c.clip_rect.y) * sy;
    float x1 = std::max(0.0f, c.clip_rect.x + c.clip_rect.w) * sx;
    float y1 = std::max(0.0f, c.clip_rect.y + c.clip_rect.h) * sy;
    scissor.offset = {static_cast<int32_t>(x0), static_cast<int32_t>(y0)};
    scissor.extent = {static_cast<uint32_t>(std::max(0.0f, x1 - x0)),
                      static_cast<uint32_t>(std::max(0.0f, y1 - y0))};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    GpuBuffer& vb = c.is_text ? fb.text_vtx : fb.quad_vtx;
    GpuBuffer& ib = c.is_text ? fb.text_idx : fb.quad_idx;
    if (!vb.buffer || !ib.buffer) continue;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, c.elem_count, 1, c.index_offset, 0, 0);
  }
}

ugui::TextureId GuiRenderBackend::CreateTexture(uint32_t width, uint32_t height,
                                                ugui::RHIFormat format, const void* pixels,
                                                ugui::RHIFilter filter) {
  if (!info_.device || width == 0 || height == 0 || !pixels) return ugui::kNullTextureId;
  UserTexture ut;
  ut.width = width;
  ut.height = height;
  ut.sampler = filter == ugui::RHIFilter::kNearest ? nearest_sampler_ : linear_sampler_;
  RhiFormatToVk(format, ut.fmt, ut.pixel_size);
  ut.tex = MakeTexture(width, height, ut.fmt, ut.pixel_size, pixels, ut.sampler);
  if (ut.tex.image == VK_NULL_HANDLE) return ugui::kNullTextureId;
  ugui::TextureId id = next_user_id_++;
  user_textures_.emplace(id, ut);
  return id;
}

void GuiRenderBackend::UpdateTexture(ugui::TextureId id, const void* pixels) {
  auto it = user_textures_.find(id);
  if (it == user_textures_.end() || !pixels) return;
  UserTexture& ut = it->second;
  vkDeviceWaitIdle(info_.device);
  FreeTexture(ut.tex);
  ut.tex = MakeTexture(ut.width, ut.height, ut.fmt, ut.pixel_size, pixels, ut.sampler);
}

void GuiRenderBackend::DestroyTexture(ugui::TextureId id) {
  auto it = user_textures_.find(id);
  if (it == user_textures_.end()) return;
  vkDeviceWaitIdle(info_.device);
  FreeTexture(it->second.tex);
  user_textures_.erase(it);
}

}  // namespace rx::ui

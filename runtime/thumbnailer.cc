#include "thumbnailer.h"

#if defined(RECREATION_HAS_UGUI)

#include <volk.h>

#include <cstddef>
#include <cstring>

#include <stb_image.h>
#include <stb_image_write.h>

#include "asset/mesh.h"
#include "core/log.h"
#include "core/math.h"
#include "render/core/renderer.h"
#include "render/rhi/device.h"
#include "render/rhi/vulkan_interop.h"
#include "render/util/shader_util.h"
#include "shader_pack.h"
#include "shaders/thumb_ps_hlsl.h"
#include "shaders/thumb_vs_hlsl.h"

namespace rx {
namespace {

struct PushData {
  float mvp[16];
  float albedo[4];
  float light[4];
};

}  // namespace

struct Thumbnailer::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t qfam = 0;
  int size = 128;
  bool ready = false;

  VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
  VkDeviceMemory color_mem = VK_NULL_HANDLE, depth_mem = VK_NULL_HANDLE;
  VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
  VkBuffer vbuf = VK_NULL_HANDLE, ibuf = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
  VkDeviceMemory vmem = VK_NULL_HANDLE, imem = VK_NULL_HANDLE, rmem = VK_NULL_HANDLE;
  VkDeviceSize vcap = 0, icap = 0;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  uint32_t FindMemory(uint32_t filter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
  }

  bool CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                    VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &ci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(device, buf, &reqs);
    VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = FindMemory(reqs.memoryTypeBits, props);
    if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, buf, mem, 0);
    return true;
  }

  // Uploads bytes into a host-visible buffer, growing it if needed.
  void UploadHostBuffer(VkBuffer& buf, VkDeviceMemory& mem, VkDeviceSize& cap,
                        VkBufferUsageFlags usage, const void* src, VkDeviceSize bytes) {
    if (cap < bytes) {
      if (buf) vkDestroyBuffer(device, buf, nullptr);
      if (mem) vkFreeMemory(device, mem, nullptr);
      buf = VK_NULL_HANDLE;
      mem = VK_NULL_HANDLE;
      VkDeviceSize want = bytes + bytes / 2 + 4096;
      CreateBuffer(want, usage,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf,
                   mem);
      cap = want;
    }
    void* dst = nullptr;
    vkMapMemory(device, mem, 0, bytes, 0, &dst);
    std::memcpy(dst, src, static_cast<size_t>(bytes));
    vkUnmapMemory(device, mem);
  }

  bool CreateImage(VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& img,
                   VkDeviceMemory& mem, VkImageView& view) {
    VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent = {static_cast<uint32_t>(size), static_cast<uint32_t>(size), 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.format = fmt;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = usage;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    if (vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device, img, &reqs);
    VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = FindMemory(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindImageMemory(device, img, mem, 0);
    VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(device, &vi, nullptr, &view) == VK_SUCCESS;
  }

  bool CreatePipeline() {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size = sizeof(PushData);
    VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device, &li, nullptr, &layout) != VK_SUCCESS) return false;

    // Blobs come from the mounted shaders:// archive, falling back to the bytes
    // embedded in the binary when the pack is missing (shaderpack::Load).
    const base::Vector<u8> vs_blob =
        shaderpack::Load("thumb.vs", k_thumb_vs_hlsl, sizeof(k_thumb_vs_hlsl));
    const base::Vector<u8> fs_blob =
        shaderpack::Load("thumb.ps", k_thumb_ps_hlsl, sizeof(k_thumb_ps_hlsl));
    VkShaderModule vs = render::CreateShaderModule(device, vs_blob.data(), vs_blob.size());
    VkShaderModule fs = render::CreateShaderModule(device, fs_blob.data(), fs_blob.size());
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(asset::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)},
    };
    VkPipelineVertexInputStateCreateInfo vin{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = 2;
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
    VkPipelineDepthStencilStateCreateInfo depthss{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthss.depthTestEnable = VK_TRUE;
    depthss.depthWriteEnable = VK_TRUE;
    depthss.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba{};
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

    VkFormat color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT;
    VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_fmt;
    rendering.depthAttachmentFormat = depth_fmt;

    VkGraphicsPipelineCreateInfo pi{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rendering;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &depthss;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = layout;
    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    return r == VK_SUCCESS;
  }

  void Barrier(VkCommandBuffer c, VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
               VkImageLayout to, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
               VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(c, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  }
};

Thumbnailer::Thumbnailer() : impl_(std::make_unique<Impl>()) {}
Thumbnailer::~Thumbnailer() { Shutdown(); }
bool Thumbnailer::ready() const { return impl_ && impl_->ready; }
int Thumbnailer::size() const { return impl_ ? impl_->size : 0; }

bool Thumbnailer::Init(render::Renderer& renderer, int size) {
  render::Device* dev = renderer.device();
  if (!dev || dev->is_stub()) return false;
  // Rasterizes with raw Vulkan; null handles on other backends leave it off.
  const render::VulkanHandles vk = render::GetVulkanHandles(*dev);
  Impl& m = *impl_;
  m.device = vk.device;
  m.phys = vk.physical_device;
  m.queue = vk.graphics_queue;
  m.qfam = vk.graphics_family;
  m.size = size;
  if (!m.device || !m.queue) return false;

  if (!m.CreateImage(VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, m.color, m.color_mem, m.color_view))
    return false;
  if (!m.CreateImage(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, m.depth, m.depth_mem, m.depth_view))
    return false;
  if (!m.CreateBuffer(static_cast<VkDeviceSize>(size) * size * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m.readback, m.rmem))
    return false;
  if (!m.CreatePipeline()) return false;

  VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = m.qfam;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if (vkCreateCommandPool(m.device, &pci, nullptr, &m.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = m.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(m.device, &cai, &m.cmd) != VK_SUCCESS) return false;
  VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(m.device, &fci, nullptr, &m.fence);

  m.ready = true;
  RX_INFO("thumbnailer ready ({}x{})", size, size);
  return true;
}

void Thumbnailer::Shutdown() {
  if (!impl_) return;
  Impl& m = *impl_;
  if (m.device) {
    if (m.queue) vkQueueWaitIdle(m.queue);
    if (m.pipeline) vkDestroyPipeline(m.device, m.pipeline, nullptr);
    if (m.layout) vkDestroyPipelineLayout(m.device, m.layout, nullptr);
    if (m.color_view) vkDestroyImageView(m.device, m.color_view, nullptr);
    if (m.depth_view) vkDestroyImageView(m.device, m.depth_view, nullptr);
    if (m.color) vkDestroyImage(m.device, m.color, nullptr);
    if (m.depth) vkDestroyImage(m.device, m.depth, nullptr);
    if (m.color_mem) vkFreeMemory(m.device, m.color_mem, nullptr);
    if (m.depth_mem) vkFreeMemory(m.device, m.depth_mem, nullptr);
    if (m.vbuf) vkDestroyBuffer(m.device, m.vbuf, nullptr);
    if (m.ibuf) vkDestroyBuffer(m.device, m.ibuf, nullptr);
    if (m.readback) vkDestroyBuffer(m.device, m.readback, nullptr);
    if (m.vmem) vkFreeMemory(m.device, m.vmem, nullptr);
    if (m.imem) vkFreeMemory(m.device, m.imem, nullptr);
    if (m.rmem) vkFreeMemory(m.device, m.rmem, nullptr);
    if (m.fence) vkDestroyFence(m.device, m.fence, nullptr);
    if (m.pool) vkDestroyCommandPool(m.device, m.pool, nullptr);
  }
  m = Impl{};
}

bool Thumbnailer::Render(const asset::Mesh& mesh, std::vector<std::uint8_t>& out) {
  Impl& m = *impl_;
  if (!m.ready || mesh.lods.empty()) return false;
  const asset::MeshLod& lod = mesh.lods[0];
  const uint32_t nv = static_cast<uint32_t>(lod.vertices.size());
  const uint32_t ni = static_cast<uint32_t>(lod.indices.size());
  if (nv == 0 || ni == 0) return false;

  m.UploadHostBuffer(m.vbuf, m.vmem, m.vcap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, lod.vertices.data(),
                     static_cast<VkDeviceSize>(nv) * sizeof(asset::Vertex));
  m.UploadHostBuffer(m.ibuf, m.imem, m.icap, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, lod.indices.data(),
                     static_cast<VkDeviceSize>(ni) * sizeof(uint32_t));

  // Frame the bounds with a 3/4 orthographic view (y-flipped for Vulkan clip).
  const Vec3 c{mesh.bounds_center[0], mesh.bounds_center[1], mesh.bounds_center[2]};
  float r = mesh.bounds_radius;
  if (!(r > 0.0f)) r = 1.0f;
  const Vec3 dir = Normalize(Vec3{0.82f, 0.62f, 1.0f});
  const float dist = r * 4.0f;
  const Vec3 eye = c + dir * dist;
  Mat4 view = LookAt(eye, c, Vec3{0, 1, 0});
  const float ext = r * 1.18f;
  Mat4 proj = Orthographic(-ext, ext, -ext, ext, 0.01f, dist + r * 4.0f);
  proj.m[5] = -proj.m[5];  // Vulkan clip space points +y down
  Mat4 mvp = proj * view;

  PushData push{};
  std::memcpy(push.mvp, mvp.m, sizeof(push.mvp));
  push.albedo[0] = 0.80f;
  push.albedo[1] = 0.79f;
  push.albedo[2] = 0.76f;
  push.albedo[3] = 1.0f;
  const Vec3 L = Normalize(Vec3{0.45f, 0.80f, 0.55f});
  push.light[0] = L.x;
  push.light[1] = L.y;
  push.light[2] = L.z;

  vkResetCommandBuffer(m.cmd, 0);
  VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(m.cmd, &bi);

  m.Barrier(m.cmd, m.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  m.Barrier(m.cmd, m.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

  VkRenderingAttachmentInfo color_att{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color_att.imageView = m.color_view;
  color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  VkRenderingAttachmentInfo depth_att{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth_att.imageView = m.depth_view;
  depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_att.clearValue.depthStencil = {1.0f, 0};

  VkRenderingInfo ri{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {static_cast<uint32_t>(m.size), static_cast<uint32_t>(m.size)}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color_att;
  ri.pDepthAttachment = &depth_att;
  vkCmdBeginRendering(m.cmd, &ri);

  VkViewport vp{0, 0, static_cast<float>(m.size), static_cast<float>(m.size), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {static_cast<uint32_t>(m.size), static_cast<uint32_t>(m.size)}};
  vkCmdSetViewport(m.cmd, 0, 1, &vp);
  vkCmdSetScissor(m.cmd, 0, 1, &sc);
  vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.pipeline);
  vkCmdPushConstants(m.cmd, m.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(PushData), &push);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(m.cmd, 0, 1, &m.vbuf, &zero);
  vkCmdBindIndexBuffer(m.cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(m.cmd, ni, 1, 0, 0, 0);
  vkCmdEndRendering(m.cmd);

  m.Barrier(m.cmd, m.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {static_cast<uint32_t>(m.size), static_cast<uint32_t>(m.size), 1};
  vkCmdCopyImageToBuffer(m.cmd, m.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.readback, 1,
                         &region);
  vkEndCommandBuffer(m.cmd);

  VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &m.cmd;
  vkResetFences(m.device, 1, &m.fence);
  if (vkQueueSubmit(m.queue, 1, &si, m.fence) != VK_SUCCESS) return false;
  vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, UINT64_MAX);

  const size_t bytes = static_cast<size_t>(m.size) * m.size * 4;
  out.resize(bytes);
  void* mapped = nullptr;
  vkMapMemory(m.device, m.rmem, 0, bytes, 0, &mapped);
  std::memcpy(out.data(), mapped, bytes);
  vkUnmapMemory(m.device, m.rmem);
  return true;
}

bool Thumbnailer::LoadCached(const std::string& path, std::vector<std::uint8_t>& out) const {
  int w = 0, h = 0, n = 0;
  unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (!data) return false;
  if (w != impl_->size || h != impl_->size) {
    stbi_image_free(data);
    return false;
  }
  out.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  return true;
}

void Thumbnailer::SaveCached(const std::string& path, const std::vector<std::uint8_t>& rgba) const {
  if (rgba.size() < static_cast<size_t>(impl_->size) * impl_->size * 4) return;
  stbi_write_png(path.c_str(), impl_->size, impl_->size, 4, rgba.data(), impl_->size * 4);
}

}  // namespace rx

#else  // !RECREATION_HAS_UGUI

namespace rx {
struct Thumbnailer::Impl {};
Thumbnailer::Thumbnailer() = default;
Thumbnailer::~Thumbnailer() = default;
bool Thumbnailer::Init(render::Renderer&, int) { return false; }
void Thumbnailer::Shutdown() {}
bool Thumbnailer::ready() const { return false; }
int Thumbnailer::size() const { return 0; }
bool Thumbnailer::Render(const asset::Mesh&, std::vector<std::uint8_t>&) { return false; }
bool Thumbnailer::LoadCached(const std::string&, std::vector<std::uint8_t>&) const { return false; }
void Thumbnailer::SaveCached(const std::string&, const std::vector<std::uint8_t>&) const {}
}  // namespace rx

#endif  // RECREATION_HAS_UGUI

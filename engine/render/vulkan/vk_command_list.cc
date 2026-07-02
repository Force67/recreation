#include "core/log.h"
#include "render/vulkan/vk_backend.h"

namespace rec::render::vk {

void VulkanCommandList::BindPipeline(PipelineHandle pipeline) {
  bound_ = Rec(pipeline);
  vkCmdBindPipeline(cmd_, bound_->bind_point, bound_->pipeline);
}

void VulkanCommandList::BindSet(u32 set_index, BindingSetHandle set) {
  const BindingSetRecord* record = Rec(set);
  vkCmdBindDescriptorSets(cmd_, bound_->bind_point, bound_->layout, set_index, 1, &record->set, 0,
                          nullptr);
}

void VulkanCommandList::BindTransient(u32 set_index, std::span<const BindingItem> items) {
  VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  info.descriptorPool = transient_pool_;
  info.descriptorSetCount = 1;
  info.pSetLayouts = &bound_->set_layouts[set_index];
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device_.device(), &info, &set) != VK_SUCCESS) {
    REC_ERROR("transient descriptor allocation failed (pool exhausted?)");
    return;
  }
  device_.WriteDescriptors(set, items);
  vkCmdBindDescriptorSets(cmd_, bound_->bind_point, bound_->layout, set_index, 1, &set, 0,
                          nullptr);
}

void VulkanCommandList::PushConstants(const void* data, u32 size, u32 offset) {
  vkCmdPushConstants(cmd_, bound_->layout, bound_->push_stages, offset, size, data);
}

void VulkanCommandList::Dispatch(u32 x, u32 y, u32 z) { vkCmdDispatch(cmd_, x, y, z); }

void VulkanCommandList::BeginRendering(const RenderingInfo& info) {
  VkRenderingAttachmentInfo colors[8];
  for (size_t i = 0; i < info.colors.size(); ++i) {
    const ColorAttachment& src = info.colors[i];
    colors[i] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colors[i].imageView = View(src.view);
    colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colors[i].loadOp = src.load == LoadOp::kLoad     ? VK_ATTACHMENT_LOAD_OP_LOAD
                       : src.load == LoadOp::kClear  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colors[i].storeOp = src.store == StoreOp::kStore ? VK_ATTACHMENT_STORE_OP_STORE
                                                     : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colors[i].clearValue.color = {{src.clear[0], src.clear[1], src.clear[2], src.clear[3]}};
  }

  VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (info.depth) {
    depth.imageView = View(info.depth->view);
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = info.depth->load == LoadOp::kLoad    ? VK_ATTACHMENT_LOAD_OP_LOAD
                   : info.depth->load == LoadOp::kClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.storeOp = info.depth->store == StoreOp::kStore ? VK_ATTACHMENT_STORE_OP_STORE
                                                         : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {info.depth->clear, 0};
  }

  VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {info.extent.width, info.extent.height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = static_cast<u32>(info.colors.size());
  rendering.pColorAttachments = colors;
  rendering.pDepthAttachment = info.depth ? &depth : nullptr;
  vkCmdBeginRendering(cmd_, &rendering);

  SetViewport(0, 0, static_cast<f32>(info.extent.width), static_cast<f32>(info.extent.height));
  SetScissor(0, 0, info.extent.width, info.extent.height);
}

void VulkanCommandList::EndRendering() { vkCmdEndRendering(cmd_); }

void VulkanCommandList::SetViewport(f32 x, f32 y, f32 width, f32 height) {
  VkViewport viewport{x, y, width, height, 0.0f, 1.0f};
  vkCmdSetViewport(cmd_, 0, 1, &viewport);
}

void VulkanCommandList::SetScissor(i32 x, i32 y, u32 width, u32 height) {
  VkRect2D scissor{{x, y}, {width, height}};
  vkCmdSetScissor(cmd_, 0, 1, &scissor);
}

void VulkanCommandList::BindVertexBuffer(u32 binding, const GpuBuffer& buffer, u64 offset) {
  VkBuffer handle = Rec(buffer.handle)->buffer;
  vkCmdBindVertexBuffers(cmd_, binding, 1, &handle, &offset);
}

void VulkanCommandList::BindIndexBuffer(const GpuBuffer& buffer, u64 offset, IndexType type) {
  vkCmdBindIndexBuffer(cmd_, Rec(buffer.handle)->buffer, offset,
                       type == IndexType::kUint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::Draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                             u32 first_instance) {
  vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanCommandList::DrawIndexed(u32 index_count, u32 instance_count, u32 first_index,
                                    i32 vertex_offset, u32 first_instance) {
  vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VulkanCommandList::DrawIndexedIndirect(const GpuBuffer& args, u64 offset, u32 draw_count,
                                            u32 stride) {
  vkCmdDrawIndexedIndirect(cmd_, Rec(args.handle)->buffer, offset, draw_count, stride);
}

void VulkanCommandList::DrawMeshTasks(u32 x, u32 y, u32 z) {
  vkCmdDrawMeshTasksEXT(cmd_, x, y, z);
}

void VulkanCommandList::TextureBarriers(std::span<const TextureBarrier> barriers) {
  VkImageMemoryBarrier2 image_barriers[16];
  size_t offset = 0;
  while (offset < barriers.size()) {
    size_t batch = std::min<size_t>(barriers.size() - offset, 16);
    for (size_t i = 0; i < batch; ++i) {
      const TextureBarrier& src = barriers[offset + i];
      const TextureRecord* texture = Rec(src.texture);
      StateInfo before = StateInfoOf(src.before, true);
      StateInfo after = StateInfoOf(src.after, false);
      VkImageMemoryBarrier2& b = image_barriers[i];
      b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = before.stages;
      b.srcAccessMask = before.access;
      b.dstStageMask = after.stages;
      b.dstAccessMask = after.access;
      b.oldLayout = before.layout;
      b.newLayout = after.layout;
      b.image = texture->image;
      b.subresourceRange = {texture->aspect, src.base_mip,
                            src.mip_count == 0 ? VK_REMAINING_MIP_LEVELS : src.mip_count, 0,
                            VK_REMAINING_ARRAY_LAYERS};
    }
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<u32>(batch);
    dep.pImageMemoryBarriers = image_barriers;
    vkCmdPipelineBarrier2(cmd_, &dep);
    offset += batch;
  }
}

void VulkanCommandList::MemoryBarrier(BarrierScope src, BarrierScope dst) {
  ScopeInfo from = ScopeInfoOf(src, true);
  ScopeInfo to = ScopeInfoOf(dst, false);
  VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = from.stages;
  barrier.srcAccessMask = from.access;
  barrier.dstStageMask = to.stages;
  barrier.dstAccessMask = to.access;
  VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd_, &dep);
}

void VulkanCommandList::CopyBufferToTexture(const GpuBuffer& src, const GpuImage& dst,
                                            std::span<const BufferTextureCopy> regions) {
  const TextureRecord* texture = Rec(dst.handle);
  base::Vector<VkBufferImageCopy> copies(regions.size());
  for (size_t i = 0; i < regions.size(); ++i) {
    const BufferTextureCopy& r = regions[i];
    u32 width = r.extent.width ? r.extent.width : std::max(dst.extent.width >> r.mip, 1u);
    u32 height = r.extent.height ? r.extent.height : std::max(dst.extent.height >> r.mip, 1u);
    copies[i] = {};
    copies[i].bufferOffset = r.buffer_offset;
    copies[i].imageSubresource = {texture->aspect, r.mip, r.array_layer, 1};
    copies[i].imageExtent = {width, height, 1};
  }
  vkCmdCopyBufferToImage(cmd_, Rec(src.handle)->buffer, texture->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(copies.size()),
                         copies.data());
}

void VulkanCommandList::CopyTextureToBuffer(const GpuImage& src, const GpuBuffer& dst,
                                            const BufferTextureCopy& region) {
  const TextureRecord* texture = Rec(src.handle);
  u32 width = region.extent.width ? region.extent.width
                                  : std::max(src.extent.width >> region.mip, 1u);
  u32 height = region.extent.height ? region.extent.height
                                    : std::max(src.extent.height >> region.mip, 1u);
  VkBufferImageCopy copy{};
  copy.bufferOffset = region.buffer_offset;
  copy.imageSubresource = {texture->aspect, region.mip, region.array_layer, 1};
  copy.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd_, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         Rec(dst.handle)->buffer, 1, &copy);
}

void VulkanCommandList::CopyBuffer(const GpuBuffer& src, u64 src_offset, const GpuBuffer& dst,
                                   u64 dst_offset, u64 size) {
  VkBufferCopy region{src_offset, dst_offset, size};
  vkCmdCopyBuffer(cmd_, Rec(src.handle)->buffer, Rec(dst.handle)->buffer, 1, &region);
}

void VulkanCommandList::BlitMip(const GpuImage& image, u32 src_mip, Extent2D src_extent,
                                u32 dst_mip, Extent2D dst_extent) {
  const TextureRecord* texture = Rec(image.handle);
  VkImageBlit blit{};
  blit.srcSubresource = {texture->aspect, src_mip, 0, 1};
  blit.srcOffsets[1] = {static_cast<i32>(src_extent.width), static_cast<i32>(src_extent.height),
                        1};
  blit.dstSubresource = {texture->aspect, dst_mip, 0, 1};
  blit.dstOffsets[1] = {static_cast<i32>(dst_extent.width), static_cast<i32>(dst_extent.height),
                        1};
  vkCmdBlitImage(cmd_, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
}

void VulkanCommandList::ClearColor(const GpuImage& image, const f32 color[4]) {
  const TextureRecord* texture = Rec(image.handle);
  VkClearColorValue value{{color[0], color[1], color[2], color[3]}};
  VkImageSubresourceRange range{texture->aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                                VK_REMAINING_ARRAY_LAYERS};
  vkCmdClearColorImage(cmd_, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                       &range);
}

void VulkanCommandList::ClearDepth(const GpuImage& image, f32 depth) {
  const TextureRecord* texture = Rec(image.handle);
  VkClearDepthStencilValue value{depth, 0};
  VkImageSubresourceRange range{texture->aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                                VK_REMAINING_ARRAY_LAYERS};
  vkCmdClearDepthStencilImage(cmd_, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value,
                              1, &range);
}

void VulkanCommandList::FillBuffer(const GpuBuffer& buffer, u64 offset, u64 size, u32 data) {
  vkCmdFillBuffer(cmd_, Rec(buffer.handle)->buffer, offset, size, data);
}

namespace {

// Shared by build and size query so the geometry description cannot drift
// between the two (a mismatch is a validation error and a GPU hang).
struct BlasGeometry {
  base::Vector<VkAccelerationStructureGeometryKHR> geometries;
  base::Vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  base::Vector<u32> primitive_counts;
};

BlasGeometry TranslateBlas(const BlasBuildDesc& desc) {
  BlasGeometry result;
  for (const AccelTriangles& t : desc.geometries) {
    VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    auto& tri = geometry.geometry.triangles;
    tri = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.vertexFormat = ToVkFormat(t.vertex_format);
    tri.vertexData.deviceAddress = t.vertex_address;
    tri.vertexStride = t.vertex_stride;
    tri.maxVertex = t.vertex_count == 0 ? 0 : t.vertex_count - 1;
    tri.indexType = t.index_type == IndexType::kUint16 ? VK_INDEX_TYPE_UINT16
                                                       : VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = t.index_address;
    result.geometries.push_back(geometry);
    result.ranges.push_back({.primitiveCount = t.index_count / 3});
    result.primitive_counts.push_back(t.index_count / 3);
  }
  return result;
}

VkAccelerationStructureBuildGeometryInfoKHR BlasBuildInfo(
    const BlasBuildDesc& desc, const BlasGeometry& geometry) {
  VkAccelerationStructureBuildGeometryInfoKHR info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  info.flags = desc.fast_trace ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                               : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  info.geometryCount = static_cast<u32>(geometry.geometries.size());
  info.pGeometries = geometry.geometries.data();
  return info;
}

}  // namespace

void VulkanCommandList::BuildBlas(AccelStructHandle blas, const BlasBuildDesc& desc,
                                  const GpuBuffer& scratch, u64 scratch_offset) {
  AccelStructRecord* record = Rec(blas);
  BlasGeometry geometry = TranslateBlas(desc);
  VkAccelerationStructureBuildGeometryInfoKHR info = BlasBuildInfo(desc, geometry);
  info.dstAccelerationStructure = record->accel;
  info.scratchData.deviceAddress = scratch.address + scratch_offset;
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = geometry.ranges.data();
  vkCmdBuildAccelerationStructuresKHR(cmd_, 1, &info, &ranges);
}

void VulkanCommandList::BuildTlas(AccelStructHandle tlas, const GpuBuffer& instances,
                                  u32 instance_count, const GpuBuffer& scratch) {
  AccelStructRecord* record = Rec(tlas);
  VkAccelerationStructureGeometryKHR geometry{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
  geometry.geometry.instances.data.deviceAddress = instances.address;

  VkAccelerationStructureBuildGeometryInfoKHR info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  info.geometryCount = 1;
  info.pGeometries = &geometry;
  info.dstAccelerationStructure = record->accel;
  // The caller sizes the scratch buffer with alignment slack; align here so
  // every backend gets a correctly aligned scratch base.
  u64 alignment = device_.caps().accel_scratch_alignment;
  info.scratchData.deviceAddress = (scratch.address + alignment - 1) & ~(alignment - 1);

  VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = instance_count};
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  vkCmdBuildAccelerationStructuresKHR(cmd_, 1, &info, &ranges);
}

void VulkanCommandList::ResetTimestamps(TimestampPoolHandle pool, u32 first, u32 count) {
  vkCmdResetQueryPool(cmd_, Rec(pool)->pool, first, count);
}

void VulkanCommandList::WriteTimestamp(TimestampPoolHandle pool, u32 index, bool after_work) {
  vkCmdWriteTimestamp(cmd_,
                      after_work ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      Rec(pool)->pool, index);
}

void VulkanCommandList::BeginDebugLabel(const char* name) {
  if (!device_.caps().debug_utils) return;
  VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
  label.pLabelName = name;
  vkCmdBeginDebugUtilsLabelEXT(cmd_, &label);
}

void VulkanCommandList::EndDebugLabel() {
  if (!device_.caps().debug_utils) return;
  vkCmdEndDebugUtilsLabelEXT(cmd_);
}

}  // namespace rec::render::vk

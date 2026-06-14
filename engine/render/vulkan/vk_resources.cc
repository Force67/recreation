#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"

namespace rec::render {

bool Device::InitResources() {
  VmaVulkanFunctions functions{};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo info{};
  info.instance = instance_;
  info.physicalDevice = physical_device_;
  info.device = device_;
  info.vulkanApiVersion = VK_API_VERSION_1_3;
  info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  info.pVulkanFunctions = &functions;
  if (vmaCreateAllocator(&info, &allocator_) != VK_SUCCESS) {
    REC_ERROR("vma allocator creation failed");
    return false;
  }

  VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = graphics_family_;
  vkCreateCommandPool(device_, &pool_info, nullptr, &immediate_pool_);

  VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(device_, &fence_info, nullptr, &immediate_fence_);
  return true;
}

void Device::ShutdownResources() {
  if (immediate_fence_) vkDestroyFence(device_, immediate_fence_, nullptr);
  if (immediate_pool_) vkDestroyCommandPool(device_, immediate_pool_, nullptr);
  if (allocator_) vmaDestroyAllocator(allocator_);
  immediate_fence_ = VK_NULL_HANDLE;
  immediate_pool_ = VK_NULL_HANDLE;
  allocator_ = nullptr;
}

void Device::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
  VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = immediate_pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device_, &alloc, &cmd);

  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  record(cmd);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(graphics_queue_, 1, &submit, immediate_fence_);
  vkWaitForFences(device_, 1, &immediate_fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &immediate_fence_);
  vkFreeCommandBuffers(device_, immediate_pool_, 1, &cmd);
}

GpuBuffer Device::CreateBuffer(u64 size, VkBufferUsageFlags usage, bool host_visible) {
  VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;

  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
  if (host_visible) {
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  GpuBuffer buffer;
  buffer.size = size;
  VmaAllocationInfo result{};
  if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &buffer.buffer, &buffer.allocation,
                      &result) != VK_SUCCESS) {
    REC_ERROR("buffer allocation failed ({} bytes)", size);
    return {};
  }
  buffer.mapped = result.pMappedData;
  return buffer;
}

GpuBuffer Device::CreateBufferWithData(ByteSpan data, VkBufferUsageFlags usage) {
  GpuBuffer staging = CreateBuffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
  if (!staging.mapped) return {};
  std::memcpy(staging.mapped, data.data(), data.size());

  GpuBuffer buffer = CreateBuffer(data.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  ImmediateSubmit([&](VkCommandBuffer cmd) {
    VkBufferCopy region{.srcOffset = 0, .dstOffset = 0, .size = data.size()};
    vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &region);
  });
  DestroyBuffer(staging);
  return buffer;
}

void Device::DestroyBuffer(GpuBuffer& buffer) {
  if (buffer.buffer) vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
  buffer = {};
}

GpuImage Device::CreateImage2D(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, u32 mip_levels) {
  VkImageCreateInfo image_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {extent.width, extent.height, 1};
  image_info.mipLevels = mip_levels;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.usage = usage;

  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
  alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // attachments

  GpuImage image;
  image.format = format;
  image.extent = extent;
  if (vmaCreateImage(allocator_, &image_info, &alloc_info, &image.image, &image.allocation,
                     nullptr) != VK_SUCCESS) {
    REC_ERROR("image allocation failed");
    return {};
  }

  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {aspect, 0, mip_levels, 0, 1};
  vkCreateImageView(device_, &view_info, nullptr, &image.view);
  return image;
}

GpuImage Device::CreateImageCube(VkFormat format, u32 size, VkImageUsageFlags usage,
                                 u32 mip_levels) {
  VkImageCreateInfo image_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {size, size, 1};
  image_info.mipLevels = mip_levels;
  image_info.arrayLayers = 6;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.usage = usage;

  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

  GpuImage image;
  image.format = format;
  image.extent = {size, size};
  if (vmaCreateImage(allocator_, &image_info, &alloc_info, &image.image, &image.allocation,
                     nullptr) != VK_SUCCESS) {
    REC_ERROR("cube image allocation failed");
    return {};
  }

  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  view_info.format = format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 6};
  vkCreateImageView(device_, &view_info, nullptr, &image.view);
  return image;
}

void Device::DestroyImage(GpuImage& image) {
  if (image.view) vkDestroyImageView(device_, image.view, nullptr);
  if (image.image) vmaDestroyImage(allocator_, image.image, image.allocation);
  image = {};
}

}  // namespace rec::render

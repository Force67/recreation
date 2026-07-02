#include "render/vulkan/vk_backend.h"

namespace rec::render::vk {
namespace {

// Indexed by Format; kept in declaration order and size-checked so adding a
// format without extending the table fails at compile time.
constexpr VkFormat kFormatTable[] = {
    VK_FORMAT_UNDEFINED,                 // kUnknown
    VK_FORMAT_R8_UNORM,                  // kR8Unorm
    VK_FORMAT_R8_SNORM,                  // kR8Snorm
    VK_FORMAT_R8_UINT,                   // kR8Uint
    VK_FORMAT_R8G8_UNORM,                // kRG8Unorm
    VK_FORMAT_R8G8B8A8_UNORM,            // kRGBA8Unorm
    VK_FORMAT_R8G8B8A8_SNORM,            // kRGBA8Snorm
    VK_FORMAT_R8G8B8A8_UINT,             // kRGBA8Uint
    VK_FORMAT_R8G8B8A8_SRGB,             // kRGBA8Srgb
    VK_FORMAT_B8G8R8A8_UNORM,            // kBGRA8Unorm
    VK_FORMAT_R16_UNORM,                 // kR16Unorm
    VK_FORMAT_R16_SNORM,                 // kR16Snorm
    VK_FORMAT_R16_UINT,                  // kR16Uint
    VK_FORMAT_R16_SFLOAT,                // kR16Float
    VK_FORMAT_R16G16_UNORM,              // kRG16Unorm
    VK_FORMAT_R16G16_SNORM,              // kRG16Snorm
    VK_FORMAT_R16G16_SFLOAT,             // kRG16Float
    VK_FORMAT_R16G16B16A16_UNORM,        // kRGBA16Unorm
    VK_FORMAT_R16G16B16A16_SNORM,        // kRGBA16Snorm
    VK_FORMAT_R16G16B16A16_SFLOAT,       // kRGBA16Float
    VK_FORMAT_R32_UINT,                  // kR32Uint
    VK_FORMAT_R32_SFLOAT,                // kR32Float
    VK_FORMAT_R32G32_SFLOAT,             // kRG32Float
    VK_FORMAT_R32G32B32_SFLOAT,          // kRGB32Float
    VK_FORMAT_R32G32B32A32_SFLOAT,       // kRGBA32Float
    VK_FORMAT_A2B10G10R10_UNORM_PACK32,  // kRGB10A2Unorm
    VK_FORMAT_B10G11R11_UFLOAT_PACK32,   // kRG11B10Float
    VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,    // kRGB9E5Float
    VK_FORMAT_D32_SFLOAT,                // kD32Float
    VK_FORMAT_BC1_RGB_UNORM_BLOCK,       // kBC1RgbUnorm
    VK_FORMAT_BC1_RGB_SRGB_BLOCK,        // kBC1RgbSrgb
    VK_FORMAT_BC2_UNORM_BLOCK,           // kBC2Unorm
    VK_FORMAT_BC2_SRGB_BLOCK,            // kBC2Srgb
    VK_FORMAT_BC3_UNORM_BLOCK,           // kBC3Unorm
    VK_FORMAT_BC3_SRGB_BLOCK,            // kBC3Srgb
    VK_FORMAT_BC4_UNORM_BLOCK,           // kBC4Unorm
    VK_FORMAT_BC5_UNORM_BLOCK,           // kBC5Unorm
    VK_FORMAT_BC7_UNORM_BLOCK,           // kBC7Unorm
    VK_FORMAT_BC7_SRGB_BLOCK,            // kBC7Srgb
};
static_assert(sizeof(kFormatTable) / sizeof(kFormatTable[0]) == kFormatCount,
              "format table out of sync with rhi::Format");

}  // namespace

VkFormat ToVkFormat(Format format) { return kFormatTable[static_cast<u32>(format)]; }

Format FromVkFormat(VkFormat format) {
  for (u32 i = 0; i < kFormatCount; ++i) {
    if (kFormatTable[i] == format) return static_cast<Format>(i);
  }
  return Format::kUnknown;
}

VkImageAspectFlags AspectOf(Format format) {
  return IsDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkBufferUsageFlags ToVkBufferUsage(BufferUsageFlags usage) {
  VkBufferUsageFlags result = 0;
  if (usage & kBufferUsageVertex) result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (usage & kBufferUsageIndex) result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (usage & kBufferUsageUniform) result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (usage & kBufferUsageStorage) result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage & kBufferUsageIndirect) result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (usage & kBufferUsageTransferSrc) result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (usage & kBufferUsageTransferDst) result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (usage & kBufferUsageDeviceAddress) result |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (usage & kBufferUsageAccelBuildInput) {
    result |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  if (usage & kBufferUsageAccelStorage) {
    result |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  if (usage & kBufferUsageAccelScratch) {
    result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  return result;
}

VkImageUsageFlags ToVkImageUsage(TextureUsageFlags usage, Format format) {
  VkImageUsageFlags result = 0;
  if (usage & kTextureUsageSampled) result |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (usage & kTextureUsageStorage) result |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (usage & kTextureUsageColorTarget) result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (usage & kTextureUsageDepthTarget) result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage & kTextureUsageTransferSrc) result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (usage & kTextureUsageTransferDst) result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  (void)format;
  return result;
}

VkShaderStageFlags ToVkStages(ShaderStageFlags stages) {
  VkShaderStageFlags result = 0;
  if (stages & kShaderStageVertex) result |= VK_SHADER_STAGE_VERTEX_BIT;
  if (stages & kShaderStageFragment) result |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if (stages & kShaderStageCompute) result |= VK_SHADER_STAGE_COMPUTE_BIT;
  if (stages & kShaderStageTask) result |= VK_SHADER_STAGE_TASK_BIT_EXT;
  if (stages & kShaderStageMesh) result |= VK_SHADER_STAGE_MESH_BIT_EXT;
  return result;
}

VkDescriptorType ToVkDescriptorType(BindingType type) {
  switch (type) {
    case BindingType::kStorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case BindingType::kSampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::kCombinedTextureSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case BindingType::kSampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case BindingType::kUniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::kStorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case BindingType::kAccelStruct: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  }
  return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}

VkCompareOp ToVkCompare(CompareOp op) {
  switch (op) {
    case CompareOp::kNever: return VK_COMPARE_OP_NEVER;
    case CompareOp::kLess: return VK_COMPARE_OP_LESS;
    case CompareOp::kLessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::kEqual: return VK_COMPARE_OP_EQUAL;
    case CompareOp::kGreater: return VK_COMPARE_OP_GREATER;
    case CompareOp::kGreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::kAlways: return VK_COMPARE_OP_ALWAYS;
  }
  return VK_COMPARE_OP_ALWAYS;
}

StateInfo StateInfoOf(ResourceState state, bool as_source) {
  switch (state) {
    case ResourceState::kUndefined:
      // As a source: contents are discardable, but the physical image may be
      // recycled from a transient pool while the previous frame still reads
      // it, so order behind everything.
      return {VK_IMAGE_LAYOUT_UNDEFINED,
              as_source ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
              as_source ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_NONE};
    case ResourceState::kGeneral:
      return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case ResourceState::kShaderReadCompute:
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceState::kShaderReadFragment:
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceState::kShaderReadTaskMesh:
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceState::kShaderReadAll:
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceState::kColorTarget:
      return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case ResourceState::kDepthTarget:
      return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case ResourceState::kCopySrc:
      return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT};
    case ResourceState::kCopyDst:
      return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case ResourceState::kPresent:
      return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              as_source ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                        : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
              VK_ACCESS_2_NONE};
  }
  return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
}

ScopeInfo ScopeInfoOf(BarrierScope scope, bool as_source) {
  switch (scope) {
    case BarrierScope::kNone:
      return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    case BarrierScope::kAllCommands:
      return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    case BarrierScope::kComputeWrite:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case BarrierScope::kComputeRead:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                  VK_ACCESS_2_UNIFORM_READ_BIT};
    case BarrierScope::kGraphicsRead:
      return {VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT};
    case BarrierScope::kIndirectArgs:
      return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    case BarrierScope::kTransferWrite:
      return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case BarrierScope::kTransferRead:
      return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    case BarrierScope::kAccelBuildWrite:
      return {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    case BarrierScope::kAccelRead:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    case BarrierScope::kHostRead:
      return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
  }
  (void)as_source;
  return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
}

}  // namespace rec::render::vk

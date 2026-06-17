#include "render/material_system.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace rec::render {
namespace {

struct FormatInfo {
  VkFormat vk = VK_FORMAT_UNDEFINED;
  u32 block_bytes = 0;
  u32 block_dim = 1;  // 1 for uncompressed, 4 for BCn
};

FormatInfo FormatFor(asset::TextureFormat format, bool srgb) {
  switch (format) {
    case asset::TextureFormat::kRgba8:
      return {srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4, 1};
    case asset::TextureFormat::kBc1:
      return {srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK, 8, 4};
    case asset::TextureFormat::kBc3:
      return {srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK, 16, 4};
    case asset::TextureFormat::kBc4:
      return {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4};
    case asset::TextureFormat::kBc5:
      return {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4};
    case asset::TextureFormat::kBc7:
      return {srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, 16, 4};
    case asset::TextureFormat::kUnknown:
      return {};
  }
  return {};
}

u32 MipSizeBytes(const FormatInfo& info, u32 width, u32 height) {
  u32 blocks_x = (width + info.block_dim - 1) / info.block_dim;
  u32 blocks_y = (height + info.block_dim - 1) / info.block_dim;
  return blocks_x * blocks_y * info.block_bytes;
}

u32 FullMipChainLength(u32 width, u32 height) {
  u32 levels = 1;
  while (width > 1 || height > 1) {
    width = std::max(1u, width / 2);
    height = std::max(1u, height / 2);
    ++levels;
  }
  return levels;
}

void Barrier(VkCommandBuffer cmd, VkImage image, u32 base_mip, u32 mip_count,
             VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
             VkAccessFlags2 dst_access) {
  VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0, 1};
  VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

std::unique_ptr<MaterialSystem> MaterialSystem::Create(Device& device,
                                                       BindlessRegistry* registry) {
  auto system = std::unique_ptr<MaterialSystem>(new MaterialSystem(device));
  system->registry_ = registry;

  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  if (device.caps().max_anisotropy > 1.0f) {
    sampler_info.anisotropyEnable = VK_TRUE;
    sampler_info.maxAnisotropy = std::min(16.0f, device.caps().max_anisotropy);
  }
  if (vkCreateSampler(device.device(), &sampler_info, nullptr, &system->sampler_) != VK_SUCCESS) {
    return nullptr;
  }

  VkDescriptorSetLayoutBinding bindings[5]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  for (u32 i = 1; i < 5; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 5;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &system->set_layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }

  if (!system->CreateDefaults()) return nullptr;
  return system;
}

bool MaterialSystem::CreateDefaults() {
  auto make_pixel = [&](u8 r, u8 g, u8 b, u8 a, bool srgb) {
    asset::Texture texture;
    texture.format = asset::TextureFormat::kRgba8;
    texture.width = 1;
    texture.height = 1;
    texture.is_srgb = srgb;
    texture.data.resize(4);
    texture.data[0] = r;
    texture.data[1] = g;
    texture.data[2] = b;
    texture.data[3] = a;
    return texture;
  };

  // Direct uploads, bypassing the hash map: id 0 would collide.
  asset::Texture white = make_pixel(255, 255, 255, 255, true);
  asset::Texture normal = make_pixel(128, 128, 255, 255, false);
  white_ = UploadTextureImage(white);
  flat_normal_ = UploadTextureImage(normal);
  if (white_.image == VK_NULL_HANDLE || flat_normal_.image == VK_NULL_HANDLE) return false;

  asset::Material default_material;
  default_material.base_color_factor[0] = 0.6f;
  default_material.base_color_factor[1] = 0.6f;
  default_material.base_color_factor[2] = 0.65f;
  default_material.roughness_factor = 0.8f;
  if (registry_) {
    BindlessRegistry::MaterialRecord record;
    std::memcpy(record.base_color_factor, default_material.base_color_factor, sizeof(f32) * 4);
    registry_->RegisterMaterial(record);  // index 0, the fallback
  }
  default_set_ = AllocateSet();
  if (default_set_ == VK_NULL_HANDLE) return false;
  return WriteSet(default_set_, 0, default_material);
}

GpuImage MaterialSystem::UploadTextureImage(const asset::Texture& texture) {
  FormatInfo info = FormatFor(texture.format, texture.is_srgb);
  if (info.vk == VK_FORMAT_UNDEFINED || texture.width == 0 || texture.height == 0) {
    REC_WARN("texture upload skipped, unsupported format");
    return {};
  }
  if (texture.array_layers != 1 || texture.is_cubemap) {
    REC_WARN("array/cubemap textures not supported yet");
    return {};
  }

  // Uncompressed single-mip sources get a generated chain; BCn ships its
  // mips in the asset and cannot be blitted.
  bool generate_mips = texture.mip_count == 1 && info.block_dim == 1;
  u32 mip_count = generate_mips ? FullMipChainLength(texture.width, texture.height)
                                : texture.mip_count;

  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (generate_mips) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  GpuImage image = device_.CreateImage2D(info.vk, {texture.width, texture.height}, usage,
                                         VK_IMAGE_ASPECT_COLOR_BIT, mip_count);
  if (image.image == VK_NULL_HANDLE) return {};

  GpuBuffer staging =
      device_.CreateBuffer(texture.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
  if (!staging.mapped) {
    device_.DestroyImage(image);
    return {};
  }
  std::memcpy(staging.mapped, texture.data.data(), texture.data.size());

  u32 upload_mips = generate_mips ? 1 : mip_count;
  device_.ImmediateSubmit([&](VkCommandBuffer cmd) {
    Barrier(cmd, image.image, 0, mip_count, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    u64 offset = 0;
    u32 width = texture.width;
    u32 height = texture.height;
    for (u32 mip = 0; mip < upload_mips; ++mip) {
      VkBufferImageCopy region{};
      region.bufferOffset = offset;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
      region.imageExtent = {width, height, 1};
      vkCmdCopyBufferToImage(cmd, staging.buffer, image.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      offset += MipSizeBytes(info, width, height);
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }

    if (generate_mips && mip_count > 1) {
      i32 src_width = static_cast<i32>(texture.width);
      i32 src_height = static_cast<i32>(texture.height);
      for (u32 mip = 1; mip < mip_count; ++mip) {
        Barrier(cmd, image.image, mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        i32 dst_width = std::max(1, src_width / 2);
        i32 dst_height = std::max(1, src_height / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        blit.srcOffsets[1] = {src_width, src_height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        blit.dstOffsets[1] = {dst_width, dst_height, 1};
        vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        src_width = dst_width;
        src_height = dst_height;
      }
      // Mips 0..n-2 sit in TRANSFER_SRC after feeding the next level, the
      // last one still in TRANSFER_DST.
      Barrier(cmd, image.image, 0, mip_count - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      Barrier(cmd, image.image, mip_count - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else {
      Barrier(cmd, image.image, 0, mip_count, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
  });
  device_.DestroyBuffer(staging);
  return image;
}

bool MaterialSystem::UploadTexture(const asset::Texture& texture) {
  if (textures_.find(texture.id.hash)) return true;
  GpuImage image = UploadTextureImage(texture);
  if (image.image == VK_NULL_HANDLE) return false;
  textures_.insert(texture.id.hash, image);
  if (registry_ && texture.is_srgb) {
    // Only color textures matter for ray hit shading.
    u32 index = registry_->RegisterTexture(image.view);
    if (index != BindlessRegistry::kInvalidIndex) {
      bindless_textures_.insert(texture.id.hash, index);
    }
  }
  return true;
}

bool MaterialSystem::AddPool() {
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaterialsPerPool},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaterialsPerPool * 4},
  };
  VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.maxSets = kMaterialsPerPool;
  info.poolSizeCount = 2;
  info.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (vkCreateDescriptorPool(device_.device(), &info, nullptr, &pool) != VK_SUCCESS) return false;
  pools_.push_back(pool);
  sets_in_last_pool_ = 0;

  GpuBuffer params = device_.CreateBuffer(static_cast<u64>(kParamStride) * kMaterialsPerPool,
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
  if (!params.mapped) return false;
  param_buffers_.push_back(params);
  return true;
}

VkDescriptorSet MaterialSystem::AllocateSet() {
  if (pools_.empty() || sets_in_last_pool_ == kMaterialsPerPool) {
    if (!AddPool()) return VK_NULL_HANDLE;
  }
  VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  info.descriptorPool = pools_.back();
  info.descriptorSetCount = 1;
  info.pSetLayouts = &set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device_.device(), &info, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  ++sets_in_last_pool_;
  return set;
}

const GpuImage* MaterialSystem::texture_or(u64 hash, const GpuImage& fallback) const {
  if (hash != 0) {
    if (const GpuImage* found = textures_.find(hash)) return found;
  }
  return &fallback;
}

bool MaterialSystem::WriteSet(VkDescriptorSet set, u32 param_index,
                              const asset::Material& material) {
  Params params;
  std::memcpy(params.base_color_factor, material.base_color_factor, sizeof(f32) * 4);
  std::memcpy(params.emissive_factor, material.emissive_factor, sizeof(f32) * 3);
  params.metallic_factor = material.metallic_factor;
  params.roughness_factor = material.roughness_factor;
  params.alpha_cutoff = material.alpha_cutoff;
  // Blend materials draw without the cutout test; mask materials cut.
  if (material.alpha_mode == asset::AlphaMode::kMask) params.flags |= kFlagAlphaMask;
  if (material.normal && textures_.find(material.normal.hash)) {
    params.flags |= kFlagHasNormalMap;
  }

  GpuBuffer& buffer = param_buffers_.back();
  u64 offset = static_cast<u64>(param_index) * kParamStride;
  std::memcpy(static_cast<u8*>(buffer.mapped) + offset, &params, sizeof(params));

  VkDescriptorBufferInfo buffer_info{buffer.buffer, offset, sizeof(Params)};
  VkDescriptorImageInfo images[4];
  const GpuImage* maps[4] = {
      texture_or(material.base_color.hash, white_),
      texture_or(material.normal.hash, flat_normal_),
      texture_or(material.metallic_roughness.hash, white_),
      texture_or(material.emissive.hash, white_),
  };
  VkWriteDescriptorSet writes[5];
  writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[0].dstSet = set;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[0].pBufferInfo = &buffer_info;
  for (u32 i = 0; i < 4; ++i) {
    images[i] = {sampler_, maps[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[i + 1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i + 1].dstSet = set;
    writes[i + 1].dstBinding = i + 1;
    writes[i + 1].descriptorCount = 1;
    writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i + 1].pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(device_.device(), 5, writes, 0, nullptr);
  return true;
}

bool MaterialSystem::UploadMaterial(const asset::Material& material) {
  if (sets_.find(material.id.hash)) return true;
  VkDescriptorSet set = AllocateSet();
  if (set == VK_NULL_HANDLE) return false;
  if (!WriteSet(set, sets_in_last_pool_ - 1, material)) return false;
  sets_.insert(material.id.hash, set);
  blend_modes_.insert(material.id.hash, static_cast<u8>(material.alpha_mode));
  if (material.is_water) water_.insert(material.id.hash, 1);
  if (registry_) {
    BindlessRegistry::MaterialRecord record;
    std::memcpy(record.base_color_factor, material.base_color_factor, sizeof(f32) * 4);
    std::memcpy(record.emissive, material.emissive_factor, sizeof(f32) * 3);
    if (const u32* texture = bindless_textures_.find(material.base_color.hash)) {
      record.base_color_texture = *texture;
    }
    u32 index = registry_->RegisterMaterial(record);
    if (index != BindlessRegistry::kInvalidIndex) {
      bindless_materials_.insert(material.id.hash, index);
    }
  }
  return true;
}

bool MaterialSystem::is_water(u64 material_hash) const {
  return water_.find(material_hash) != nullptr;
}

u32 MaterialSystem::bindless_material(u64 material_hash) const {
  if (const u32* index = bindless_materials_.find(material_hash)) return *index;
  return 0;
}

bool MaterialSystem::is_blend(u64 material_hash) const {
  if (const u8* mode = blend_modes_.find(material_hash)) {
    return static_cast<asset::AlphaMode>(*mode) == asset::AlphaMode::kBlend;
  }
  return false;
}

VkDescriptorSet MaterialSystem::set(u64 material_hash) const {
  if (material_hash != 0) {
    if (const VkDescriptorSet* found = sets_.find(material_hash)) return *found;
  }
  return default_set_;
}

MaterialSystem::~MaterialSystem() {
  for (auto kv : textures_) device_.DestroyImage(kv.value);
  textures_.clear();
  device_.DestroyImage(white_);
  device_.DestroyImage(flat_normal_);
  for (GpuBuffer& buffer : param_buffers_) device_.DestroyBuffer(buffer);
  for (VkDescriptorPool pool : pools_) vkDestroyDescriptorPool(device_.device(), pool, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_.device(), set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device_.device(), sampler_, nullptr);
}

}  // namespace rec::render

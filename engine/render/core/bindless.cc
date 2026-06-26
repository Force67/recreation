#include "render/core/bindless.h"

#include <cstring>

#include "core/log.h"

namespace rec::render {

std::unique_ptr<BindlessRegistry> BindlessRegistry::Create(Device& device) {
  auto registry = std::unique_ptr<BindlessRegistry>(new BindlessRegistry(device));
  if (!registry->Initialize()) return nullptr;
  return registry;
}

bool BindlessRegistry::Initialize() {
  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  if (vkCreateSampler(device_.device(), &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }

  mesh_table_ = device_.CreateBuffer(sizeof(MeshRecord) * kMaxMeshes,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
  geometry_table_ = device_.CreateBuffer(sizeof(GeometryRecord) * kMaxGeometries,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
  material_table_ = device_.CreateBuffer(sizeof(MaterialRecord) * kMaxMaterials,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
  if (!mesh_table_.mapped || !geometry_table_.mapped || !material_table_.mapped) return false;

  // Hit shading runs from ddgi compute and the water fragment shader.
  VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutBinding bindings[5]{};
  for (u32 i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = stages;
  }
  bindings[3].binding = 3;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[3].descriptorCount = kMaxTextures;
  bindings[3].stageFlags = stages;
  bindings[4].binding = 4;
  bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[4].descriptorCount = 1;
  bindings[4].stageFlags = stages;

  VkDescriptorBindingFlags binding_flags[5] = {
      0, 0, 0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      0};
  VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  flags_info.bindingCount = 5;
  flags_info.pBindingFlags = binding_flags;

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.pNext = &flags_info;
  set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  set_info.bindingCount = 5;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device_.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
  };
  VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(device_.device(), &pool_info, nullptr, &pool_) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &set_layout_;
  if (vkAllocateDescriptorSets(device_.device(), &alloc, &set_) != VK_SUCCESS) return false;

  VkDescriptorBufferInfo buffers[3] = {
      {mesh_table_.buffer, 0, mesh_table_.size},
      {geometry_table_.buffer, 0, geometry_table_.size},
      {material_table_.buffer, 0, material_table_.size},
  };
  VkDescriptorImageInfo sampler_write{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet writes[4];
  for (u32 i = 0; i < 3; ++i) {
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffers[i];
  }
  writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[3].dstSet = set_;
  writes[3].dstBinding = 4;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[3].pImageInfo = &sampler_write;
  vkUpdateDescriptorSets(device_.device(), 4, writes, 0, nullptr);
  return true;
}

u32 BindlessRegistry::RegisterTexture(VkImageView view) {
  if (texture_count_ >= kMaxTextures) {
    REC_WARN("bindless texture table full");
    return kInvalidIndex;
  }
  u32 index = texture_count_++;
  VkDescriptorImageInfo image{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set_;
  write.dstBinding = 3;
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image;
  vkUpdateDescriptorSets(device_.device(), 1, &write, 0, nullptr);
  return index;
}

u32 BindlessRegistry::RegisterMaterial(const MaterialRecord& record) {
  if (material_count_ >= kMaxMaterials) {
    REC_WARN("bindless material table full");
    return kInvalidIndex;
  }
  u32 index = material_count_++;
  std::memcpy(static_cast<u8*>(material_table_.mapped) + index * sizeof(MaterialRecord), &record,
              sizeof(record));
  return index;
}

u32 BindlessRegistry::RegisterMesh(VkBuffer vertices, VkBuffer indices,
                                   const GeometryRecord* geometries, u32 geometry_count) {
  if (mesh_count_ >= kMaxMeshes || geometry_count_ + geometry_count > kMaxGeometries) {
    REC_WARN("bindless mesh tables full");
    return kInvalidIndex;
  }
  auto address = [&](VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device_.device(), &info);
  };
  MeshRecord record;
  record.vertex_address = address(vertices);
  record.index_address = address(indices);
  record.geometry_offset = geometry_count_;
  std::memcpy(static_cast<u8*>(geometry_table_.mapped) +
                  geometry_count_ * sizeof(GeometryRecord),
              geometries, geometry_count * sizeof(GeometryRecord));
  geometry_count_ += geometry_count;
  u32 index = mesh_count_++;
  std::memcpy(static_cast<u8*>(mesh_table_.mapped) + index * sizeof(MeshRecord), &record,
              sizeof(record));
  return index;
}

BindlessRegistry::~BindlessRegistry() {
  device_.DestroyBuffer(mesh_table_);
  device_.DestroyBuffer(geometry_table_);
  device_.DestroyBuffer(material_table_);
  if (pool_) vkDestroyDescriptorPool(device_.device(), pool_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_.device(), set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device_.device(), sampler_, nullptr);
}

}  // namespace rec::render

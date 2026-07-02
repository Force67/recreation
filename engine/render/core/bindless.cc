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
  mesh_table_ = device_.CreateBuffer(sizeof(MeshRecord) * kMaxMeshes, kBufferUsageStorage, true);
  geometry_table_ =
      device_.CreateBuffer(sizeof(GeometryRecord) * kMaxGeometries, kBufferUsageStorage, true);
  material_table_ =
      device_.CreateBuffer(sizeof(MaterialRecord) * kMaxMaterials, kBufferUsageStorage, true);
  if (!mesh_table_.mapped || !geometry_table_.mapped || !material_table_.mapped) return false;

  // Hit shading runs from ddgi compute and the water fragment shader.
  set_layout_ = device_.CreateBindingLayout({
      .stages = kShaderStageCompute | kShaderStageFragment,
      .slots = {{0, BindingType::kStorageBuffer},
                {1, BindingType::kStorageBuffer},
                {2, BindingType::kStorageBuffer},
                {3, BindingType::kSampledImage, kMaxTextures, /*variable_count=*/true},
                {4, BindingType::kSampler}},
      .update_after_bind = true,
  });
  if (!set_layout_) return false;

  set_ = device_.CreateBindingSet(set_layout_, kMaxTextures);
  if (!set_) return false;

  SamplerHandle sampler = device_.GetSampler({});  // trilinear repeat
  device_.UpdateBindingSet(set_, {Bind::StorageBuffer(0, mesh_table_),
                                  Bind::StorageBuffer(1, geometry_table_),
                                  Bind::StorageBuffer(2, material_table_),
                                  Bind::Sampler(4, sampler)});
  return true;
}

u32 BindlessRegistry::RegisterTexture(TextureView view) {
  if (texture_count_ >= kMaxTextures) {
    REC_WARN("bindless texture table full");
    return kInvalidIndex;
  }
  u32 index = texture_count_++;
  BindingItem item = Bind::SampledView(3, view);
  item.array_index = index;
  device_.UpdateBindingSet(set_, {item});
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

u32 BindlessRegistry::RegisterMesh(const GpuBuffer& vertices, const GpuBuffer& indices,
                                   const GeometryRecord* geometries, u32 geometry_count) {
  if (mesh_count_ >= kMaxMeshes || geometry_count_ + geometry_count > kMaxGeometries) {
    REC_WARN("bindless mesh tables full");
    return kInvalidIndex;
  }
  MeshRecord record;
  record.vertex_address = vertices.address;
  record.index_address = indices.address;
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
  device_.DestroyBindingSet(set_);
  device_.DestroyBindingLayout(set_layout_);
}

}  // namespace rec::render

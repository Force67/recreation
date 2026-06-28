#ifndef RECREATION_RENDER_BINDLESS_H_
#define RECREATION_RENDER_BINDLESS_H_

#include <memory>

#include "core/types.h"
#include "render/rhi/device.h"

namespace rec::render {

// GPU-side scene tables for ray hit shading: every uploaded mesh, geometry
// range, material and base color texture is reachable from a shader given a
// TLAS hit (instanceCustomIndex -> mesh record -> geometry record ->
// material record -> texture). Vertex and index data go through buffer
// device addresses, so only the textures need a descriptor array (update
// after bind, partially bound).
class BindlessRegistry {
 public:
  static constexpr u32 kMaxTextures = 4096;
  static constexpr u32 kMaxMeshes = 16384;
  static constexpr u32 kMaxGeometries = 1u << 17;
  static constexpr u32 kMaxMaterials = 16384;
  static constexpr u32 kInvalidIndex = 0xffffffffu;

  // Layouts match the StructuredBuffer declarations in ddgi_rays.cs.hlsl.
  struct MeshRecord {
    u64 vertex_address = 0;
    u64 index_address = 0;
    u32 geometry_offset = 0;
    u32 pad[3] = {};
  };
  struct GeometryRecord {
    u32 index_offset = 0;
    u32 material_index = 0;
  };
  struct MaterialRecord {
    f32 base_color_factor[4] = {1, 1, 1, 1};
    f32 emissive[3] = {0, 0, 0};
    u32 base_color_texture = kInvalidIndex;
    u32 flags = 0;  // bit0: alpha mask (cutout)
    f32 alpha_cutoff = 0.5f;
    u32 pad0 = 0;  // pad to 48B: the std430 array stride rounds up to a multiple
    u32 pad1 = 0;  // of 16 (float4 alignment), so the shader struct must match.
  };
  static_assert(sizeof(MaterialRecord) % 16 == 0, "bindless material stride must be 16-aligned");
  static constexpr u32 kMaterialAlphaMask = 1u << 0;

  static std::unique_ptr<BindlessRegistry> Create(Device& device);
  ~BindlessRegistry();

  BindlessRegistry(const BindlessRegistry&) = delete;
  BindlessRegistry& operator=(const BindlessRegistry&) = delete;

  // All return kInvalidIndex when the respective table is full.
  u32 RegisterTexture(VkImageView view);
  u32 RegisterMaterial(const MaterialRecord& record);
  // Geometry records must follow the blas geometry order (non-blend
  // submeshes in submesh order). Returns the instanceCustomIndex.
  u32 RegisterMesh(VkBuffer vertices, VkBuffer indices, const GeometryRecord* geometries,
                   u32 geometry_count);

  VkDescriptorSetLayout set_layout() const { return set_layout_; }
  VkDescriptorSet set() const { return set_; }

 private:
  explicit BindlessRegistry(Device& device) : device_(device) {}
  bool Initialize();

  Device& device_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;

  GpuBuffer mesh_table_;      // host visible MeshRecord[]
  GpuBuffer geometry_table_;  // host visible GeometryRecord[]
  GpuBuffer material_table_;  // host visible MaterialRecord[]
  u32 mesh_count_ = 0;
  u32 geometry_count_ = 0;
  u32 material_count_ = 0;
  u32 texture_count_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_BINDLESS_H_

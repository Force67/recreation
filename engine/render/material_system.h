#ifndef RECREATION_RENDER_MATERIAL_SYSTEM_H_
#define RECREATION_RENDER_MATERIAL_SYSTEM_H_

#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/material.h"
#include "asset/texture.h"
#include "render/bindless.h"
#include "render/rhi/device.h"

namespace rec::render {

// GPU side of the asset Material/Texture types. Owns uploaded textures, a
// shared trilinear anisotropic sampler, a parameter buffer and one
// persistent descriptor set per material (set 1 of the mesh pipeline):
//   binding 0  uniform MaterialParams
//   binding 1  base color        (srgb)
//   binding 2  normal map        (linear)
//   binding 3  metallic roughness(linear)
//   binding 4  emissive          (srgb)
// Missing maps fall back to builtin 1x1 defaults so the shader never
// branches on texture presence.
class MaterialSystem {
 public:
  // Matches the std140 block in mesh.frag.
  struct Params {
    f32 base_color_factor[4] = {1, 1, 1, 1};
    f32 emissive_factor[3] = {0, 0, 0};
    f32 metallic_factor = 0;
    f32 roughness_factor = 1;
    f32 alpha_cutoff = 0.5f;
    u32 flags = 0;
    f32 pad = 0;
    // Extended pbr lobes, one 16-byte row each (matches std140 in mesh.ps).
    f32 clearcoat = 0;
    f32 clearcoat_roughness = 0;
    f32 anisotropy = 0;
    f32 ior = 1.5f;
    f32 sheen_color[3] = {0, 0, 0};
    f32 sheen_roughness = 0.3f;
    f32 subsurface_color[3] = {0.9f, 0.3f, 0.2f};
    f32 subsurface = 0;
  };
  static constexpr u32 kFlagAlphaMask = 1u << 0;
  static constexpr u32 kFlagHasNormalMap = 1u << 1;

  // registry may be null (no raytracing); hit-shading tables are skipped.
  static std::unique_ptr<MaterialSystem> Create(Device& device, BindlessRegistry* registry);
  ~MaterialSystem();

  MaterialSystem(const MaterialSystem&) = delete;
  MaterialSystem& operator=(const MaterialSystem&) = delete;

  // Uploads pixel data and generates a full mip chain for single-mip
  // uncompressed textures. BCn data uploads its baked mips as-is.
  bool UploadTexture(const asset::Texture& texture);

  // Builds the descriptor set for a material. Referenced textures must be
  // uploaded first or they fall back to the defaults.
  bool UploadMaterial(const asset::Material& material);

  // Set for a material hash; 0 or unknown hashes get the default material.
  VkDescriptorSet set(u64 material_hash) const;

  // Blended materials draw in the sorted transparent pass instead of the
  // opaque one. Unknown hashes are opaque.
  bool is_blend(u64 material_hash) const;
  bool is_water(u64 material_hash) const;

  // Bindless material record index for ray hit shading; 0 (the default
  // material) for unknown hashes.
  u32 bindless_material(u64 material_hash) const;

  VkDescriptorSetLayout set_layout() const { return set_layout_; }
  u32 texture_count() const { return static_cast<u32>(textures_.size()); }
  u32 material_count() const { return static_cast<u32>(sets_.size()); }

 private:
  static constexpr u32 kMaterialsPerPool = 256;
  static constexpr u32 kParamStride = 256;  // covers minUniformBufferOffsetAlignment

  explicit MaterialSystem(Device& device) : device_(device) {}

  bool CreateDefaults();
  GpuImage UploadTextureImage(const asset::Texture& texture);
  bool AddPool();
  VkDescriptorSet AllocateSet();
  bool WriteSet(VkDescriptorSet set, u32 param_index, const asset::Material& material);
  const GpuImage* texture_or(u64 hash, const GpuImage& fallback) const;

  Device& device_;
  BindlessRegistry* registry_ = nullptr;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  base::Vector<VkDescriptorPool> pools_;
  u32 sets_in_last_pool_ = 0;

  base::Vector<GpuBuffer> param_buffers_;  // one per pool, host visible
  base::UnorderedMap<u64, GpuImage> textures_;
  base::UnorderedMap<u64, VkDescriptorSet> sets_;
  base::UnorderedMap<u64, u8> blend_modes_;  // asset::AlphaMode per material
  base::UnorderedMap<u64, u8> water_;        // material hash -> is_water
  base::UnorderedMap<u64, u32> bindless_textures_;   // texture hash -> registry index
  base::UnorderedMap<u64, u32> bindless_materials_;  // material hash -> registry index
  VkDescriptorSet default_set_ = VK_NULL_HANDLE;

  GpuImage white_;        // srgb-safe 1x1 white, also neutral mr/emissive
  GpuImage flat_normal_;  // 1x1 (0.5, 0.5, 1)
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_MATERIAL_SYSTEM_H_

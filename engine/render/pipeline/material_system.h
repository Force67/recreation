#ifndef RECREATION_RENDER_MATERIAL_SYSTEM_H_
#define RECREATION_RENDER_MATERIAL_SYSTEM_H_

#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/material.h"
#include "asset/texture.h"
#include "render/core/bindless.h"
#include "render/rhi/device.h"

namespace rec::render {

// GPU side of the asset Material/Texture types. Owns uploaded textures, a
// shared trilinear anisotropic sampler, a parameter buffer and one
// persistent binding set per material (set 1 of the mesh pipeline):
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
    f32 height_scale = 0;  // pom depth (uv units); 0 skips the march
    // Extended pbr lobes, one 16-byte row each (matches std140 in mesh.ps).
    f32 clearcoat = 0;
    f32 clearcoat_roughness = 0;
    f32 anisotropy = 0;
    f32 ior = 1.5f;
    f32 sheen_color[3] = {0, 0, 0};
    f32 sheen_roughness = 0.3f;
    f32 subsurface_color[3] = {0.9f, 0.3f, 0.2f};
    f32 subsurface = 0;
    f32 iridescence = 0;
    f32 iridescence_thickness = 400.0f;
    f32 transmission = 0;
    f32 irid_pad = 0;
    // Animated texture scroll rate (uv units/sec); the shader adds
    // frame.time * uv_scroll to the uv before sampling.
    f32 uv_scroll[2] = {0, 0};
    f32 scroll_pad[2] = {0, 0};
    // Effect-shader (unlit vfx) view-angle falloff: start angle, stop angle,
    // start opacity, stop opacity (dot-of-view thresholds). Only read on the
    // mesh.ps unlit branch (kFlagEffect).
    f32 effect_falloff[4] = {1, 1, 1, 1};
    // Emissive pulse from a shader controller: x frequency (Hz), y amount
    // (0..1 of the mean the emission swings). Applies to lit glow and effects.
    f32 emissive_pulse[2] = {0, 0};
    f32 effect_pad[2] = {0, 0};
  };
  static constexpr u32 kFlagAlphaMask = 1u << 0;
  static constexpr u32 kFlagHasNormalMap = 1u << 1;
  static constexpr u32 kFlagTerrain = 1u << 2;  // splat: slots are 3 layers + weight map
  static constexpr u32 kFlagWind = 1u << 3;     // vertex wind sway (cloth/foliage)
  static constexpr u32 kFlagWater = 1u << 4;    // gerstner vertex displacement
  static constexpr u32 kFlagHasHeightMap = 1u << 5;  // parallax occlusion march
  static constexpr u32 kFlagSkin = 1u << 6;          // screen-space subsurface scattering
  static constexpr u32 kFlagHair = 1u << 7;          // kajiya-kay strand specular
  static constexpr u32 kFlagVirtualAlbedo = 1u << 8;  // albedo via the virtual-texture atlas
  static constexpr u32 kFlagEffect = 1u << 9;          // unlit emissive vfx (torch flames, glows)
  static constexpr u32 kFlagEffectAdditive = 1u << 10;  // additive blend (fire) vs alpha (mist)
  static constexpr u32 kFlagEffectGrayColor = 1u << 11;  // remap luminance through the palette
  static constexpr u32 kFlagEffectGrayAlpha = 1u << 12;  // coverage from luminance
  static constexpr u32 kFlagEffectFalloff = 1u << 13;    // view-angle opacity fade

  // Looks up an uploaded texture by asset hash (null when absent). Used by
  // systems that bind textures outside the material sets (decal atlas).
  const GpuImage* find_texture(u64 hash) const;

  // registry may be null (no raytracing); hit-shading tables are skipped.
  static std::unique_ptr<MaterialSystem> Create(Device& device, BindlessRegistry* registry);
  ~MaterialSystem();

  MaterialSystem(const MaterialSystem&) = delete;
  MaterialSystem& operator=(const MaterialSystem&) = delete;

  // Uploads pixel data and generates a full mip chain for single-mip
  // uncompressed textures. BCn data uploads its baked mips as-is. id_salt
  // namespaces the texture key per content domain (asset paths collide across
  // games); 0 keeps the unsalted key.
  bool UploadTexture(const asset::Texture& texture, u64 id_salt = 0);

  // Builds the binding set for a material. Referenced textures must be
  // uploaded first or they fall back to the defaults. id_salt namespaces the
  // material key and its texture references per content domain (it must match
  // the salt the referenced textures were uploaded with); 0 keeps the unsalted
  // key.
  bool UploadMaterial(const asset::Material& material, u64 id_salt = 0);

  // Set for a material hash; 0 or unknown hashes get the default material.
  BindingSetHandle set(u64 material_hash) const;

  // Blended materials draw in the sorted transparent pass instead of the
  // opaque one. Unknown hashes are opaque.
  bool is_blend(u64 material_hash) const;
  bool is_water(u64 material_hash) const;
  // Alpha-masked (cutout) materials: kept in the tlas but flagged non-opaque so
  // ray traces can alpha-test them. Unknown hashes are opaque.
  bool is_mask(u64 material_hash) const;
  // Effect-shader (unlit vfx) materials draw through the transparent pass's
  // unlit branch; additive ones use the additive blend pipeline (fire), the
  // rest the alpha one (mist). Unknown hashes are neither.
  bool is_effect(u64 material_hash) const;
  bool is_effect_additive(u64 material_hash) const;

  // Bindless material record index for ray hit shading; 0 (the default
  // material) for unknown hashes.
  u32 bindless_material(u64 material_hash) const;
  // Bindless texture-table index for an uploaded (sRGB) texture, or
  // BindlessRegistry::kInvalidIndex when absent. Used to texture particles.
  u32 bindless_texture(u64 texture_hash) const;

  BindingLayoutHandle set_layout() const { return set_layout_; }
  u32 texture_count() const { return static_cast<u32>(textures_.size()); }
  u32 material_count() const { return static_cast<u32>(sets_.size()); }

 private:
  static constexpr u32 kMaterialsPerPool = 256;
  static constexpr u32 kParamStride = 256;  // covers minUniformBufferOffsetAlignment

  explicit MaterialSystem(Device& device) : device_(device) {}

  bool CreateDefaults();
  GpuImage UploadTextureImage(const asset::Texture& texture);
  bool AddPool();
  BindingSetHandle AllocateSet();
  bool WriteSet(BindingSetHandle set, u32 param_index, const asset::Material& material,
                u64 id_salt = 0);
  const GpuImage* texture_or(u64 hash, const GpuImage& fallback) const;

  Device& device_;
  BindlessRegistry* registry_ = nullptr;
  SamplerHandle sampler_;
  u32 sets_in_last_pool_ = 0;

  base::Vector<GpuBuffer> param_buffers_;  // one per pool, host visible
  base::UnorderedMap<u64, GpuImage> textures_;
  base::UnorderedMap<u64, BindingSetHandle> sets_;
  base::UnorderedMap<u64, u8> blend_modes_;  // asset::AlphaMode per material
  base::UnorderedMap<u64, u8> water_;        // material hash -> is_water
  base::UnorderedMap<u64, u8> effects_;      // 0 none, 1 alpha effect, 2 additive effect
  base::UnorderedMap<u64, u32> bindless_textures_;   // texture hash -> registry index
  base::UnorderedMap<u64, u32> bindless_materials_;  // material hash -> registry index
  BindingLayoutHandle set_layout_;
  BindingSetHandle default_set_;

  GpuImage white_;        // srgb-safe 1x1 white, also neutral mr/emissive
  GpuImage flat_normal_;  // 1x1 (0.5, 0.5, 1)
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_MATERIAL_SYSTEM_H_

#ifndef RECREATION_ASSET_MATERIAL_H_
#define RECREATION_ASSET_MATERIAL_H_

#include "asset/asset_id.h"
#include "core/types.h"

namespace rec::asset {

enum class AlphaMode : u8 { kOpaque, kMask, kBlend };

// PBR metallic roughness. Legacy spec/gloss materials from the Bethesda
// shader sets are approximated into this during conversion.
struct Material {
  AssetId id;
  AssetId base_color;
  AssetId normal;
  AssetId metallic_roughness;
  AssetId emissive;
  f32 base_color_factor[4] = {1, 1, 1, 1};
  f32 metallic_factor = 0;
  f32 roughness_factor = 1;
  f32 emissive_factor[3] = {0, 0, 0};
  f32 alpha_cutoff = 0.5f;
  // Extended pbr lobes (glTF KHR_materials_*). Defaults are neutral/off so a
  // plain metallic-roughness material is unchanged.
  f32 clearcoat = 0.0f;            // KHR_materials_clearcoat
  f32 clearcoat_roughness = 0.0f;
  f32 anisotropy = 0.0f;           // KHR_materials_anisotropy, -1..1
  f32 ior = 1.5f;                  // KHR_materials_ior, dielectric f0
  f32 sheen_color[3] = {0, 0, 0};  // KHR_materials_sheen
  f32 sheen_roughness = 0.3f;
  // Subsurface scattering: wrap + back-scatter translucency for skin/wax/leaves.
  f32 subsurface_color[3] = {0.9f, 0.3f, 0.2f};
  f32 subsurface = 0.0f;  // 0 = off
  // Thin-film interference (KHR_materials_iridescence): a view-angle dependent
  // rainbow on the specular, for soap bubbles, oil, beetle shells.
  f32 iridescence = 0.0f;
  f32 iridescence_thickness = 400.0f;  // film thickness in nm
  // Transmission (KHR_materials_transmission): refract the scene behind the
  // surface instead of diffusing, for glass. Routed to the transparent pass.
  f32 transmission = 0.0f;
  AlphaMode alpha_mode = AlphaMode::kOpaque;
  bool two_sided = false;
  // Routed to the dedicated water pipeline: animated waves, raytraced
  // reflections, refraction with absorption. base_color acts as the
  // absorption tint, roughness scales the wave choppiness.
  bool is_water = false;
  // Runtime terrain splat: the four texture slots are reused as three land
  // layers (base_color/normal/metallic_roughness) plus a per-cell weight map
  // (emissive). The shader tiles the layers at the native land repeat and
  // blends them by the weight map instead of the usual base-color sample.
  bool is_terrain = false;
  // Height/displacement map (r channel, 1 = surface, 0 = deepest) for
  // parallax occlusion mapping; scale is the depth in uv-tangent units.
  AssetId height;
  f32 height_scale = 0.05f;
  // Vertex wind sway (banners, curtains, foliage). Weight convention: uv.y
  // grows away from the attachment (0 = pinned edge).
  bool wind = false;
  // Skin: the scene pass exports this material's diffuse lighting to the
  // subsurface buffer and the screen-space SSS blur diffuses it (red bleed at
  // shadow edges). Independent of `subsurface` (the analytic transmission term).
  bool skin = false;
  // Hair: dual-lobe Kajiya-Kay strand specular along the vertex tangent
  // (strand direction) replaces the GGX sun response; roughness drives the
  // highlight width. Pair with alpha-masked cards for real hair.
  bool hair = false;
};

}  // namespace rec::asset

#endif  // RECREATION_ASSET_MATERIAL_H_

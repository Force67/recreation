#include "render/pipeline/material_system.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace rec::render {
namespace {

struct FormatInfo {
  Format format = Format::kUnknown;
  u32 block_bytes = 0;
  u32 block_dim = 1;  // 1 for uncompressed, 4 for BCn
};

FormatInfo FormatFor(asset::TextureFormat format, bool srgb) {
  switch (format) {
    case asset::TextureFormat::kRgba8:
      return {srgb ? Format::kRGBA8Srgb : Format::kRGBA8Unorm, 4, 1};
    case asset::TextureFormat::kBc1:
      return {srgb ? Format::kBC1RgbSrgb : Format::kBC1RgbUnorm, 8, 4};
    case asset::TextureFormat::kBc2:
      return {srgb ? Format::kBC2Srgb : Format::kBC2Unorm, 16, 4};
    case asset::TextureFormat::kBc3:
      return {srgb ? Format::kBC3Srgb : Format::kBC3Unorm, 16, 4};
    case asset::TextureFormat::kBc4:
      return {Format::kBC4Unorm, 8, 4};
    case asset::TextureFormat::kBc5:
      return {Format::kBC5Unorm, 16, 4};
    case asset::TextureFormat::kBc7:
      return {srgb ? Format::kBC7Srgb : Format::kBC7Unorm, 16, 4};
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

}  // namespace

std::unique_ptr<MaterialSystem> MaterialSystem::Create(Device& device,
                                                       BindlessRegistry* registry) {
  auto system = std::unique_ptr<MaterialSystem>(new MaterialSystem(device));
  system->registry_ = registry;

  // Trilinear repeat sampler, anisotropic when the device supports it. Cached
  // by the device, never destroyed here.
  SamplerDesc sampler_desc{};
  if (device.caps().max_anisotropy > 1.0f) {
    sampler_desc.max_anisotropy = std::min(16.0f, device.caps().max_anisotropy);
  }
  system->sampler_ = device.GetSampler(sampler_desc);

  system->set_layout_ = device.CreateBindingLayout({
      // Vertex too: the wind sway in mesh.vs reads the material flags.
      .stages = kShaderStageVertex | kShaderStageFragment,
      .slots = {{0, BindingType::kUniformBuffer},
                {1, BindingType::kCombinedTextureSampler},
                {2, BindingType::kCombinedTextureSampler},
                {3, BindingType::kCombinedTextureSampler},
                {4, BindingType::kCombinedTextureSampler}},
  });
  if (!system->set_layout_) return nullptr;

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
  if (!white_ || !flat_normal_) return false;

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
  if (!default_set_) return false;
  return WriteSet(default_set_, 0, default_material);
}

GpuImage MaterialSystem::UploadTextureImage(const asset::Texture& texture) {
  FormatInfo info = FormatFor(texture.format, texture.is_srgb);
  if (info.format == Format::kUnknown || texture.width == 0 || texture.height == 0) {
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

  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageTransferDst;
  if (generate_mips) usage |= kTextureUsageTransferSrc;
  GpuImage image =
      device_.CreateImage2D(info.format, {texture.width, texture.height}, usage, mip_count);
  if (!image) return {};

  GpuBuffer staging = device_.CreateBuffer(texture.data.size(), kBufferUsageTransferSrc, true);
  if (!staging.mapped) {
    device_.DestroyImage(image);
    return {};
  }
  std::memcpy(staging.mapped, texture.data.data(), texture.data.size());

  u32 upload_mips = generate_mips ? 1 : mip_count;
  device_.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));

    base::Vector<BufferTextureCopy> regions;
    u64 offset = 0;
    u32 width = texture.width;
    u32 height = texture.height;
    for (u32 mip = 0; mip < upload_mips; ++mip) {
      regions.push_back({.buffer_offset = offset, .mip = mip, .extent = {width, height}});
      offset += MipSizeBytes(info, width, height);
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    cmd.CopyBufferToTexture(staging, image, {regions.data(), regions.size()});

    if (generate_mips && mip_count > 1) {
      u32 src_width = texture.width;
      u32 src_height = texture.height;
      for (u32 mip = 1; mip < mip_count; ++mip) {
        cmd.Barrier({.texture = image.handle,
                     .before = ResourceState::kCopyDst,
                     .after = ResourceState::kCopySrc,
                     .base_mip = mip - 1,
                     .mip_count = 1});
        u32 dst_width = std::max(1u, src_width / 2);
        u32 dst_height = std::max(1u, src_height / 2);
        cmd.BlitMip(image, mip - 1, {src_width, src_height}, mip, {dst_width, dst_height});
        src_width = dst_width;
        src_height = dst_height;
      }
      // Mips 0..n-2 sit in kCopySrc after feeding the next level, the last one
      // still in kCopyDst.
      TextureBarrier finals[2] = {
          {.texture = image.handle,
           .before = ResourceState::kCopySrc,
           .after = ResourceState::kShaderReadAll,
           .base_mip = 0,
           .mip_count = mip_count - 1},
          {.texture = image.handle,
           .before = ResourceState::kCopyDst,
           .after = ResourceState::kShaderReadAll,
           .base_mip = mip_count - 1,
           .mip_count = 1}};
      cmd.TextureBarriers({finals, 2});
    } else {
      cmd.Barrier({.texture = image.handle,
                   .before = ResourceState::kCopyDst,
                   .after = ResourceState::kShaderReadAll});
    }
  });
  device_.DestroyBuffer(staging);
  return image;
}

bool MaterialSystem::UploadTexture(const asset::Texture& texture, u64 id_salt) {
  u64 key = texture.id.hash ^ id_salt;
  if (textures_.find(key)) return true;
  GpuImage image = UploadTextureImage(texture);
  if (!image) return false;
  textures_.insert(key, image);
  if (registry_ && texture.is_srgb) {
    // Only color textures matter for ray hit shading.
    u32 index = registry_->RegisterTexture(image.view);
    if (index != BindlessRegistry::kInvalidIndex) {
      bindless_textures_.insert(key, index);
    }
  }
  return true;
}

bool MaterialSystem::AddPool() {
  GpuBuffer params = device_.CreateBuffer(static_cast<u64>(kParamStride) * kMaterialsPerPool,
                                          kBufferUsageUniform, true);
  if (!params.mapped) return false;
  param_buffers_.push_back(params);
  sets_in_last_pool_ = 0;
  return true;
}

BindingSetHandle MaterialSystem::AllocateSet() {
  if (param_buffers_.empty() || sets_in_last_pool_ == kMaterialsPerPool) {
    if (!AddPool()) return {};
  }
  BindingSetHandle set = device_.CreateBindingSet(set_layout_);
  if (!set) return {};
  ++sets_in_last_pool_;
  return set;
}

const GpuImage* MaterialSystem::texture_or(u64 hash, const GpuImage& fallback) const {
  if (hash != 0) {
    if (const GpuImage* found = textures_.find(hash)) return found;
  }
  return &fallback;
}

bool MaterialSystem::WriteSet(BindingSetHandle set, u32 param_index,
                              const asset::Material& material, u64 id_salt) {
  Params params;
  std::memcpy(params.base_color_factor, material.base_color_factor, sizeof(f32) * 4);
  std::memcpy(params.emissive_factor, material.emissive_factor, sizeof(f32) * 3);
  params.metallic_factor = material.metallic_factor;
  params.roughness_factor = material.roughness_factor;
  params.alpha_cutoff = material.alpha_cutoff;
  params.clearcoat = material.clearcoat;
  params.clearcoat_roughness = material.clearcoat_roughness;
  params.anisotropy = material.anisotropy;
  params.ior = material.ior;
  std::memcpy(params.sheen_color, material.sheen_color, sizeof(f32) * 3);
  params.sheen_roughness = material.sheen_roughness;
  std::memcpy(params.subsurface_color, material.subsurface_color, sizeof(f32) * 3);
  params.subsurface = material.subsurface;
  params.iridescence = material.iridescence;
  params.iridescence_thickness = material.iridescence_thickness;
  params.transmission = material.transmission;
  // Blend materials draw without the cutout test; mask materials cut.
  if (material.alpha_mode == asset::AlphaMode::kMask) params.flags |= kFlagAlphaMask;
  if (material.wind) params.flags |= kFlagWind;
  if (material.is_water) params.flags |= kFlagWater;
  // Terrain reuses the normal slot as a land layer, so the normal-map path must
  // stay off; the shader branches on kFlagTerrain instead.
  if (material.is_terrain) {
    params.flags |= kFlagTerrain;
  } else if (material.normal && textures_.find(material.normal.hash ^ id_salt)) {
    params.flags |= kFlagHasNormalMap;
  }

  GpuBuffer& buffer = param_buffers_.back();
  u64 offset = static_cast<u64>(param_index) * kParamStride;
  std::memcpy(static_cast<u8*>(buffer.mapped) + offset, &params, sizeof(params));

  const GpuImage* maps[4] = {
      texture_or(material.base_color.hash ^ id_salt, white_),
      texture_or(material.normal.hash ^ id_salt, flat_normal_),
      texture_or(material.metallic_roughness.hash ^ id_salt, white_),
      texture_or(material.emissive.hash ^ id_salt, white_),
  };
  device_.UpdateBindingSet(set, {Bind::Uniform(0, buffer, offset, sizeof(Params)),
                                 Bind::Combined(1, maps[0]->view, sampler_),
                                 Bind::Combined(2, maps[1]->view, sampler_),
                                 Bind::Combined(3, maps[2]->view, sampler_),
                                 Bind::Combined(4, maps[3]->view, sampler_)});
  return true;
}

bool MaterialSystem::UploadMaterial(const asset::Material& material, u64 id_salt) {
  u64 key = material.id.hash ^ id_salt;
  if (sets_.find(key)) return true;
  BindingSetHandle set = AllocateSet();
  if (!set) return false;
  if (!WriteSet(set, sets_in_last_pool_ - 1, material, id_salt)) return false;
  sets_.insert(key, set);
  // Transmissive (glass) materials route to the transparent pass so they can
  // sample the opaque scene behind them, regardless of their declared alpha.
  asset::AlphaMode mode =
      material.transmission > 0.0f ? asset::AlphaMode::kBlend : material.alpha_mode;
  blend_modes_.insert(key, static_cast<u8>(mode));
  if (material.is_water) water_.insert(key, 1);
  if (registry_) {
    BindlessRegistry::MaterialRecord record;
    std::memcpy(record.base_color_factor, material.base_color_factor, sizeof(f32) * 4);
    std::memcpy(record.emissive, material.emissive_factor, sizeof(f32) * 3);
    record.roughness = material.roughness_factor;
    record.metallic = material.metallic_factor;
    if (const u32* texture = bindless_textures_.find(material.base_color.hash ^ id_salt)) {
      record.base_color_texture = *texture;
    }
    // The metallic-roughness map is linear, so UploadTexture skipped the bindless
    // table (it only registers sRGB color maps). Register it on demand here so the
    // path tracer can read per-texel gloss for its specular lobe.
    u64 mr_key = material.metallic_roughness.hash ^ id_salt;
    if (const u32* mr = bindless_textures_.find(mr_key)) {
      record.metallic_roughness_texture = *mr;
    } else if (const GpuImage* mr_img = textures_.find(mr_key)) {
      u32 idx = registry_->RegisterTexture(mr_img->view);
      if (idx != BindlessRegistry::kInvalidIndex) {
        bindless_textures_.insert(mr_key, idx);
        record.metallic_roughness_texture = idx;
      }
    }
    if (mode == asset::AlphaMode::kMask) {
      record.flags |= BindlessRegistry::kMaterialAlphaMask;
      record.alpha_cutoff = material.alpha_cutoff;
    }
    // Terrain splat: the rasterizer reuses the normal/emissive slots as land
    // layer 1 and the per-cell weight map. Mirror that into the bindless record
    // so the path tracer can reproduce the blend (base_color = layer 0,
    // metallic_roughness = layer 2 are already registered above).
    if (material.is_terrain) {
      record.flags |= BindlessRegistry::kMaterialTerrain;
      if (const u32* l1 = bindless_textures_.find(material.normal.hash ^ id_salt)) {
        record.terrain_layer1_texture = *l1;
      }
      // The weight map is linear, so UploadTexture skipped the bindless table;
      // register it on demand like the metallic-roughness map above.
      u64 ctrl_key = material.emissive.hash ^ id_salt;
      if (const u32* ctrl = bindless_textures_.find(ctrl_key)) {
        record.terrain_weight_texture = *ctrl;
      } else if (const GpuImage* ctrl_img = textures_.find(ctrl_key)) {
        u32 idx = registry_->RegisterTexture(ctrl_img->view);
        if (idx != BindlessRegistry::kInvalidIndex) {
          bindless_textures_.insert(ctrl_key, idx);
          record.terrain_weight_texture = idx;
        }
      }
    }
    u32 index = registry_->RegisterMaterial(record);
    if (index != BindlessRegistry::kInvalidIndex) {
      bindless_materials_.insert(key, index);
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

bool MaterialSystem::is_mask(u64 material_hash) const {
  if (const u8* mode = blend_modes_.find(material_hash)) {
    return static_cast<asset::AlphaMode>(*mode) == asset::AlphaMode::kMask;
  }
  return false;
}

BindingSetHandle MaterialSystem::set(u64 material_hash) const {
  if (material_hash != 0) {
    if (const BindingSetHandle* found = sets_.find(material_hash)) return *found;
  }
  return default_set_;
}

MaterialSystem::~MaterialSystem() {
  for (auto kv : textures_) device_.DestroyImage(kv.value);
  textures_.clear();
  device_.DestroyImage(white_);
  device_.DestroyImage(flat_normal_);
  for (GpuBuffer& buffer : param_buffers_) device_.DestroyBuffer(buffer);
  for (auto kv : sets_) device_.DestroyBindingSet(kv.value);
  if (default_set_) device_.DestroyBindingSet(default_set_);
  if (set_layout_) device_.DestroyBindingLayout(set_layout_);
}

}  // namespace rec::render

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
                {4, BindingType::kCombinedTextureSampler},
                {5, BindingType::kCombinedTextureSampler}},
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
  u64 map_keys[5];
  return WriteSet(default_set_, static_cast<u32>(param_buffers_.size()) - 1,
                  sets_in_last_pool_ - 1, default_material, 0, map_keys);
}

GpuImage MaterialSystem::UploadTextureImage(const asset::Texture& texture, u32 first_mip) {
  FormatInfo info = FormatFor(texture.format, texture.is_srgb);
  if (info.format == Format::kUnknown || texture.width == 0 || texture.height == 0) {
    REC_WARN("texture upload skipped, unsupported format");
    return {};
  }
  if (texture.array_layers != 1 || texture.is_cubemap) {
    REC_WARN("array/cubemap textures not supported yet");
    return {};
  }
  if (first_mip >= texture.mip_count) return {};

  // Uncompressed single-mip sources get a generated chain; BCn ships its
  // mips in the asset and cannot be blitted. Partial (streamed) uploads only
  // happen on baked chains, never through the generate path.
  bool generate_mips = texture.mip_count == 1 && info.block_dim == 1;
  u32 top_width = std::max(1u, texture.width >> first_mip);
  u32 top_height = std::max(1u, texture.height >> first_mip);
  u32 mip_count = generate_mips ? FullMipChainLength(texture.width, texture.height)
                                : texture.mip_count - first_mip;

  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageTransferDst;
  if (generate_mips) usage |= kTextureUsageTransferSrc;
  GpuImage image =
      device_.CreateImage2D(info.format, {top_width, top_height}, usage, mip_count);
  if (!image) return {};

  // Skip past the source mips above the resident range.
  u64 skip = 0;
  {
    u32 width = texture.width;
    u32 height = texture.height;
    for (u32 mip = 0; mip < first_mip; ++mip) {
      skip += MipSizeBytes(info, width, height);
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
  }
  u64 upload_bytes = texture.data.size() - skip;
  GpuBuffer staging = device_.CreateBuffer(upload_bytes, kBufferUsageTransferSrc, true);
  if (!staging.mapped) {
    device_.DestroyImage(image);
    return {};
  }
  std::memcpy(staging.mapped, texture.data.data() + skip, upload_bytes);

  u32 upload_mips = generate_mips ? 1 : mip_count;
  device_.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));

    base::Vector<BufferTextureCopy> regions;
    u64 offset = 0;
    u32 width = top_width;
    u32 height = top_height;
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

u64 MaterialSystem::BytesForMips(const asset::Texture& texture, u32 first_mip) const {
  FormatInfo info = FormatFor(texture.format, texture.is_srgb);
  u64 bytes = 0;
  u32 width = texture.width;
  u32 height = texture.height;
  for (u32 mip = 0; mip < texture.mip_count; ++mip) {
    if (mip >= first_mip) bytes += MipSizeBytes(info, width, height);
    width = std::max(1u, width / 2);
    height = std::max(1u, height / 2);
  }
  return bytes;
}

bool MaterialSystem::UploadTexture(const asset::Texture& texture, u64 id_salt) {
  u64 key = texture.id.hash ^ id_salt;
  if (textures_.find(key)) return true;
  GpuImage image = UploadTextureImage(texture);
  if (!image) return false;

  auto record = std::make_unique<TextureRecord>();
  record->key = key;
  record->image = image;
  record->total_mips = texture.mip_count;
  FormatInfo info = FormatFor(texture.format, texture.is_srgb);
  bool baked = info.block_dim == 4 && texture.mip_count > 1;
  // Generated chains add ~1/3 on top of the source mip 0.
  record->full_bytes = baked ? texture.data.size() : texture.data.size() * 4 / 3;
  record->resident_bytes = record->full_bytes;
  if (baked) {
    u32 dim = std::max(texture.width, texture.height);
    u32 tail = 0;
    while (tail + 1 < texture.mip_count && (dim >> tail) > kTailMaxDim) ++tail;
    record->tail_first_mip = tail;
    record->streamable = tail > 0;
    if (record->streamable) record->source = texture;  // retained for re-promotes
  }
  // Fresh uploads count as hot so an over-budget load burst can't demote a
  // texture before the material/mesh referencing it even arrives.
  record->last_used = current_frame_;
  if (registry_ && texture.is_srgb) {
    // Only color textures matter for ray hit shading.
    record->bindless = registry_->RegisterTexture(image.view);
  }
  resident_bytes_ += record->resident_bytes;
  u32 index = static_cast<u32>(texture_records_.size());
  texture_records_.push_back(std::move(record));
  textures_.insert(key, index);
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

MaterialSystem::TextureRecord* MaterialSystem::record_for(u64 hash) {
  if (hash == 0) return nullptr;
  const u32* index = textures_.find(hash);
  return index ? texture_records_[*index].get() : nullptr;
}

const GpuImage* MaterialSystem::texture_or(u64 hash, const GpuImage& fallback) const {
  if (hash != 0) {
    if (const u32* index = textures_.find(hash)) return &texture_records_[*index]->image;
  }
  return &fallback;
}

u32 MaterialSystem::EnsureBindless(u64 key) {
  TextureRecord* record = record_for(key);
  if (!record || !registry_) return BindlessRegistry::kInvalidIndex;
  if (record->bindless == BindlessRegistry::kInvalidIndex) {
    record->bindless = registry_->RegisterTexture(record->image.view);
  }
  return record->bindless;
}

void MaterialSystem::WriteSetBindings(BindingSetHandle set, const MaterialRuntime& runtime) {
  const GpuImage* maps[5] = {
      texture_or(runtime.map_keys[0], white_),
      texture_or(runtime.map_keys[1], flat_normal_),
      texture_or(runtime.map_keys[2], white_),
      texture_or(runtime.map_keys[3], white_),
      texture_or(runtime.map_keys[4], white_),  // white = surface level
  };
  GpuBuffer& buffer = param_buffers_[runtime.pool];
  u64 offset = static_cast<u64>(runtime.param_index) * kParamStride;
  device_.UpdateBindingSet(set, {Bind::Uniform(0, buffer, offset, sizeof(Params)),
                                 Bind::Combined(1, maps[0]->view, sampler_),
                                 Bind::Combined(2, maps[1]->view, sampler_),
                                 Bind::Combined(3, maps[2]->view, sampler_),
                                 Bind::Combined(4, maps[3]->view, sampler_),
                                 Bind::Combined(5, maps[4]->view, sampler_)});
}

bool MaterialSystem::WriteSet(BindingSetHandle set, u32 pool, u32 param_index,
                              const asset::Material& material, u64 id_salt,
                              u64 out_map_keys[5]) {
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
  params.uv_scroll[0] = material.uv_scroll_u;
  params.uv_scroll[1] = material.uv_scroll_v;
  params.emissive_pulse[0] = material.emissive_pulse[0];
  params.emissive_pulse[1] = material.emissive_pulse[1];
  // Effect-shader (unlit vfx) geometry: torch/campfire flames, glow planes,
  // mist. base_color is the source texture, base_color_factor the emissive
  // colour * multiple, and the unlit shader branch reads these flags/params.
  if (material.effect) {
    params.flags |= kFlagEffect;
    if (material.effect_additive) params.flags |= kFlagEffectAdditive;
    if (material.effect_grayscale_color) params.flags |= kFlagEffectGrayColor;
    if (material.effect_grayscale_alpha) params.flags |= kFlagEffectGrayAlpha;
    if (material.effect_falloff) params.flags |= kFlagEffectFalloff;
    for (int k = 0; k < 4; ++k) params.effect_falloff[k] = material.effect_falloff_params[k];
  }
  // Blend materials draw without the cutout test; mask materials cut.
  if (material.alpha_mode == asset::AlphaMode::kMask) params.flags |= kFlagAlphaMask;
  if (material.wind) params.flags |= kFlagWind;
  if (material.is_water) params.flags |= kFlagWater;
  if (material.skin) params.flags |= kFlagSkin;
  if (material.hair) params.flags |= kFlagHair;
  if (material.virtual_albedo) params.flags |= kFlagVirtualAlbedo;
  // Terrain reuses the normal slot as a land layer, so the normal-map path must
  // stay off; the shader branches on kFlagTerrain instead.
  if (material.is_terrain) {
    params.flags |= kFlagTerrain;
  } else if (material.normal && textures_.find(material.normal.hash ^ id_salt)) {
    params.flags |= kFlagHasNormalMap;
    if (material.normal_model_space) params.flags |= kFlagNormalModelSpace;
  }
  if (material.height && textures_.find(material.height.hash ^ id_salt)) {
    params.flags |= kFlagHasHeightMap;
    params.height_scale = material.height_scale;
  }
  // Terrain splat v2: resolve the palette to bindless indices. The indices
  // live in this uniform, so the layers are pinned - a streamed slot move
  // would leave the params stale. Any unresolvable layer keeps the whole
  // material on the legacy 3-layer path (flag unset).
  if (material.is_terrain && material.terrain_layer_count > 0 && registry_) {
    u32 count = std::min(material.terrain_layer_count, 8u);
    bool complete = true;
    for (u32 s = 0; s < count && complete; ++s) {
      u64 albedo_key = material.terrain_layers[s].hash ^ id_salt;
      Pin(albedo_key);
      u32 albedo = EnsureBindless(albedo_key);
      if (albedo == BindlessRegistry::kInvalidIndex) {
        complete = false;
        break;
      }
      params.terrain_albedo[s] = albedo;
      if (material.terrain_layer_normals[s]) {
        u64 normal_key = material.terrain_layer_normals[s].hash ^ id_salt;
        Pin(normal_key);
        params.terrain_normal[s] = EnsureBindless(normal_key);
      }
    }
    if (complete) {
      params.flags |= kFlagTerrainV2;
    } else {
      REC_WARN("terrain v2 material {:x}: palette layer missing from bindless, using 3-layer path",
               material.id.hash);
    }
  }

  GpuBuffer& buffer = param_buffers_[pool];
  u64 offset = static_cast<u64>(param_index) * kParamStride;
  std::memcpy(static_cast<u8*>(buffer.mapped) + offset, &params, sizeof(params));

  MaterialRuntime runtime;
  runtime.set = set;
  runtime.pool = pool;
  runtime.param_index = param_index;
  runtime.map_keys[0] = material.base_color.hash ^ id_salt;
  runtime.map_keys[1] = material.normal.hash ^ id_salt;
  runtime.map_keys[2] = material.metallic_roughness.hash ^ id_salt;
  runtime.map_keys[3] = material.emissive.hash ^ id_salt;
  runtime.map_keys[4] = material.height.hash ^ id_salt;
  WriteSetBindings(set, runtime);
  std::memcpy(out_map_keys, runtime.map_keys, sizeof(runtime.map_keys));
  return true;
}

const GpuImage* MaterialSystem::find_texture(u64 hash) const {
  if (const u32* index = textures_.find(hash)) return &texture_records_[*index]->image;
  return nullptr;
}

bool MaterialSystem::UploadMaterial(const asset::Material& material, u64 id_salt) {
  u64 key = material.id.hash ^ id_salt;
  if (sets_.find(key)) return true;
  BindingSetHandle set = AllocateSet();
  if (!set) return false;
  MaterialRuntime runtime;
  runtime.set = set;
  runtime.pool = static_cast<u32>(param_buffers_.size()) - 1;
  runtime.param_index = sets_in_last_pool_ - 1;
  if (!WriteSet(set, runtime.pool, runtime.param_index, material, id_salt, runtime.map_keys)) {
    return false;
  }
  // Transmissive (glass) materials route to the transparent pass so they can
  // sample the opaque scene behind them, regardless of their declared alpha.
  asset::AlphaMode mode =
      material.transmission > 0.0f ? asset::AlphaMode::kBlend : material.alpha_mode;
  blend_modes_.insert(key, static_cast<u8>(mode));
  if (material.is_water) water_.insert(key, 1);
  if (material.effect) effects_.insert(key, material.effect_additive ? 2 : 1);
  if (registry_) {
    BindlessRegistry::MaterialRecord record;
    std::memcpy(record.base_color_factor, material.base_color_factor, sizeof(f32) * 4);
    std::memcpy(record.emissive, material.emissive_factor, sizeof(f32) * 3);
    record.roughness = material.roughness_factor;
    record.metallic = material.metallic_factor;
    if (TextureRecord* base = record_for(material.base_color.hash ^ id_salt)) {
      record.base_color_texture = base->bindless;
    }
    // The metallic-roughness map is linear, so UploadTexture skipped the bindless
    // table (it only registers sRGB color maps). Register it on demand here so the
    // path tracer can read per-texel gloss for its specular lobe.
    record.metallic_roughness_texture = EnsureBindless(material.metallic_roughness.hash ^ id_salt);
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
      if (TextureRecord* layer1 = record_for(material.normal.hash ^ id_salt)) {
        record.terrain_layer1_texture = layer1->bindless;
      }
      // The weight map is linear, so UploadTexture skipped the bindless table;
      // register it on demand like the metallic-roughness map above.
      record.terrain_weight_texture = EnsureBindless(material.emissive.hash ^ id_salt);
    }
    u32 index = registry_->RegisterMaterial(record);
    if (index != BindlessRegistry::kInvalidIndex) {
      bindless_materials_.insert(key, index);
      runtime.bindless_material = index;
    }
  }
  u32 material_index = static_cast<u32>(material_records_.size());
  material_records_.push_back(runtime);
  sets_.insert(key, material_index);
  // Reverse map for streaming: a texture swap rebuilds these materials' sets.
  for (u64 map_key : runtime.map_keys) {
    if (TextureRecord* record = record_for(map_key)) {
      // The same texture can fill several slots of one material; one rebuild
      // rewrites every slot, so record the material once.
      if (record->material_indices.empty() || record->material_indices.back() != material_index) {
        record->material_indices.push_back(material_index);
      }
    }
  }
  return true;
}

// --- texture streaming ------------------------------------------------------

void MaterialSystem::Pin(u64 texture_hash) {
  TextureRecord* record = record_for(texture_hash);
  if (!record) return;
  record->pinned = true;
  // Pinned consumers cache the view/bindless index, so a demoted texture must
  // come back to full residency (and then never move again).
  if (record->streamable && record->resident_first_mip != 0) {
    if (SwapResident(*record, 0, current_frame_)) ++promotes_;
  }
}

void MaterialSystem::Touch(u64 material_hash, u32 frame_index) {
  if (material_hash == 0 || budget_bytes_ == 0) return;
  const u32* index = sets_.find(material_hash);
  if (!index) return;
  MaterialRuntime& runtime = material_records_[*index];
  if (runtime.last_used == frame_index) return;
  runtime.last_used = frame_index;
  for (u64 map_key : runtime.map_keys) {
    if (TextureRecord* record = record_for(map_key)) record->last_used = frame_index;
  }
}

void MaterialSystem::BeginFrame(u32 frame_index) {
  current_frame_ = frame_index;
  // Device::BeginFrame just waited the frame ring's fence, so anything retired
  // kMaxFramesInFlight frames ago is no longer referenced by the GPU.
  size_t kept = 0;
  for (size_t i = 0; i < retired_.size(); ++i) {
    Retired& retired = retired_[i];
    if (retired.frame + Device::kMaxFramesInFlight <= frame_index) {
      if (retired.image) device_.DestroyImage(retired.image);
      if (retired.set) device_.DestroyBindingSet(retired.set);
      if (registry_ && retired.bindless_slot != BindlessRegistry::kInvalidIndex) {
        registry_->ReleaseTexture(retired.bindless_slot);
      }
    } else {
      retired_[kept++] = retired;
    }
  }
  retired_.resize(kept);
}

bool MaterialSystem::SwapResident(TextureRecord& record, u32 first_mip, u32 frame_index) {
  GpuImage next = UploadTextureImage(record.source, first_mip);
  if (!next) return false;

  u32 old_slot = record.bindless;
  u32 new_slot = BindlessRegistry::kInvalidIndex;
  if (registry_ && old_slot != BindlessRegistry::kInvalidIndex) {
    // A fresh slot: pending frames may still read the old descriptor, which
    // update-after-bind only allows us to leave alone, not rewrite.
    new_slot = registry_->RegisterTexture(next.view);
    if (new_slot == BindlessRegistry::kInvalidIndex) {
      device_.DestroyImage(next);
      return false;
    }
  }

  // Pre-create every replacement set before committing: the live sets may be
  // pending on the GPU, so a half-swapped state (image retired, set rebuild
  // failed) would leave a dangling descriptor.
  base::Vector<BindingSetHandle> fresh_sets;
  for (size_t i = 0; i < record.material_indices.size(); ++i) {
    BindingSetHandle fresh = device_.CreateBindingSet(set_layout_);
    if (!fresh) {
      for (BindingSetHandle set : fresh_sets) device_.DestroyBindingSet(set);
      if (registry_ && new_slot != BindlessRegistry::kInvalidIndex) {
        registry_->ReleaseTexture(new_slot);
      }
      device_.DestroyImage(next);
      return false;
    }
    fresh_sets.push_back(fresh);
  }

  retired_.push_back({record.image, {}, old_slot, frame_index});
  resident_bytes_ -= record.resident_bytes;
  record.image = next;
  record.resident_first_mip = first_mip;
  record.resident_bytes = BytesForMips(record.source, first_mip);
  record.bindless = new_slot;
  resident_bytes_ += record.resident_bytes;

  for (size_t i = 0; i < record.material_indices.size(); ++i) {
    MaterialRuntime& runtime = material_records_[record.material_indices[i]];
    WriteSetBindings(fresh_sets[i], runtime);
    retired_.push_back({{}, runtime.set, BindlessRegistry::kInvalidIndex, frame_index});
    runtime.set = fresh_sets[i];
    if (registry_ && runtime.bindless_material != BindlessRegistry::kInvalidIndex &&
        old_slot != BindlessRegistry::kInvalidIndex) {
      registry_->RewriteTextureIndex(runtime.bindless_material, old_slot, new_slot);
    }
  }
  return true;
}

void MaterialSystem::UpdateStreaming(u32 frame_index) {
  if (budget_bytes_ == 0 || texture_records_.empty()) return;

  // Coldest fully-resident streamable texture idle for at least min_idle
  // frames; nullptr when everything is warmer than that.
  auto coldest = [&](u32 min_idle) -> TextureRecord* {
    TextureRecord* best = nullptr;
    for (auto& entry : texture_records_) {
      TextureRecord& record = *entry;
      if (!record.streamable || record.pinned || record.resident_first_mip != 0) continue;
      if (record.last_used + min_idle > frame_index) continue;
      if (!best || record.last_used < best->last_used) best = &record;
    }
    return best;
  };

  // Promote textures whose materials drew recently, evicting cold ones first.
  u32 promotes = 0;
  for (auto& entry : texture_records_) {
    if (promotes >= kMaxPromotesPerFrame) break;
    TextureRecord& record = *entry;
    if (!record.streamable || record.resident_first_mip == 0) continue;
    if (record.last_used + kHotWindow < frame_index) continue;
    u64 needed = record.full_bytes - record.resident_bytes;
    while (resident_bytes_ + needed > budget_bytes_) {
      TextureRecord* victim = coldest(kColdWindow);
      if (!victim) break;
      if (!SwapResident(*victim, victim->tail_first_mip, frame_index)) break;
      ++demotes_;
    }
    if (resident_bytes_ + needed > budget_bytes_) continue;  // no room; stay at the tail
    if (SwapResident(record, 0, frame_index)) {
      ++promotes_;
      ++promotes;
    }
  }

  // Pressure: demote the coldest textures while over budget. Never demote a
  // texture drawn within the hot window - a working set larger than the
  // budget stays over it (warned once) instead of thrashing.
  u32 demotes = 0;
  while (resident_bytes_ > budget_bytes_ && demotes < kMaxDemotesPerFrame) {
    TextureRecord* victim = coldest(kHotWindow + 1);
    if (!victim) {
      if (!over_budget_warned_) {
        REC_WARN("texture streaming: resident {} MB over the {} MB budget with every "
                 "streamable texture hot; raise REC_TEX_BUDGET_MB",
                 resident_bytes_ >> 20, budget_bytes_ >> 20);
        over_budget_warned_ = true;
      }
      break;
    }
    if (!SwapResident(*victim, victim->tail_first_mip, frame_index)) break;
    ++demotes_;
    ++demotes;
  }
  if (resident_bytes_ <= budget_bytes_) over_budget_warned_ = false;

  // Headless-debuggable activity trace, throttled to every ~10s at 60fps.
  if ((promotes_ + demotes_) != logged_ops_ && frame_index >= logged_frame_ + 600) {
    StreamingStats stats = streaming_stats();
    REC_INFO("texture streaming: {}/{} MB resident, {}/{} textures demoted, {} promotes {} demotes",
             resident_bytes_ >> 20, budget_bytes_ >> 20, stats.demoted_count,
             stats.streamable_count, promotes_, demotes_);
    logged_ops_ = promotes_ + demotes_;
    logged_frame_ = frame_index;
  }
}

MaterialSystem::StreamingStats MaterialSystem::streaming_stats() const {
  StreamingStats stats;
  stats.resident_bytes = resident_bytes_;
  stats.budget_bytes = budget_bytes_;
  stats.promotes = promotes_;
  stats.demotes = demotes_;
  for (const auto& entry : texture_records_) {
    if (!entry->streamable) continue;
    ++stats.streamable_count;
    if (entry->resident_first_mip != 0) ++stats.demoted_count;
  }
  return stats;
}

bool MaterialSystem::is_water(u64 material_hash) const {
  return water_.find(material_hash) != nullptr;
}

u32 MaterialSystem::bindless_material(u64 material_hash) const {
  if (const u32* index = bindless_materials_.find(material_hash)) return *index;
  return 0;
}

u32 MaterialSystem::bindless_texture(u64 texture_hash) const {
  if (const u32* index = textures_.find(texture_hash)) {
    return texture_records_[*index]->bindless;
  }
  return BindlessRegistry::kInvalidIndex;
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

bool MaterialSystem::is_effect(u64 material_hash) const {
  return effects_.find(material_hash) != nullptr;
}

bool MaterialSystem::is_effect_additive(u64 material_hash) const {
  const u8* kind = effects_.find(material_hash);
  return kind && *kind == 2;
}

BindingSetHandle MaterialSystem::set(u64 material_hash) const {
  if (material_hash != 0) {
    if (const u32* index = sets_.find(material_hash)) return material_records_[*index].set;
  }
  return default_set_;
}

MaterialSystem::~MaterialSystem() {
  for (auto& record : texture_records_) device_.DestroyImage(record->image);
  texture_records_.clear();
  for (Retired& retired : retired_) {
    if (retired.image) device_.DestroyImage(retired.image);
    if (retired.set) device_.DestroyBindingSet(retired.set);
  }
  device_.DestroyImage(white_);
  device_.DestroyImage(flat_normal_);
  for (GpuBuffer& buffer : param_buffers_) device_.DestroyBuffer(buffer);
  for (MaterialRuntime& runtime : material_records_) device_.DestroyBindingSet(runtime.set);
  if (default_set_) device_.DestroyBindingSet(default_set_);
  if (set_layout_) device_.DestroyBindingLayout(set_layout_);
}

}  // namespace rec::render

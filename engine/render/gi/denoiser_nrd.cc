#include "render/gi/denoiser_nrd.h"

#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/rhi/vulkan_interop.h"
#include "render/util/shader_util.h"
#include "shaders/nrd_pack_cs_hlsl.h"

#include <NRD.h>

namespace rec::render {
namespace {

constexpr u32 kAoIdentifier = 0;
constexpr u32 kShadowIdentifier = 1;
constexpr u32 kDiffuseIdentifier = 2;
constexpr u32 kSpecularIdentifier = 3;
// Sky / invalid pixels report a viewZ beyond this so NRD ignores them.
constexpr f32 kDenoisingRange = 1.0e6f;

struct PackPush {
  f32 near_plane;
  f32 denoising_range;
  f32 pad[2];
};

Format ToFormat(nrd::Format format) {
  switch (format) {
    case nrd::Format::R8_UNORM: return Format::kR8Unorm;
    case nrd::Format::R8_SNORM: return Format::kR8Snorm;
    case nrd::Format::R8_UINT: return Format::kR8Uint;
    case nrd::Format::RG8_UNORM: return Format::kRG8Unorm;
    case nrd::Format::RGBA8_UNORM: return Format::kRGBA8Unorm;
    case nrd::Format::RGBA8_SNORM: return Format::kRGBA8Snorm;
    case nrd::Format::R16_UNORM: return Format::kR16Unorm;
    case nrd::Format::R16_SNORM: return Format::kR16Snorm;
    case nrd::Format::R16_UINT: return Format::kR16Uint;
    case nrd::Format::R16_SFLOAT: return Format::kR16Float;
    case nrd::Format::RG16_UNORM: return Format::kRG16Unorm;
    case nrd::Format::RG16_SNORM: return Format::kRG16Snorm;
    case nrd::Format::RG16_SFLOAT: return Format::kRG16Float;
    case nrd::Format::RGBA16_UNORM: return Format::kRGBA16Unorm;
    case nrd::Format::RGBA16_SNORM: return Format::kRGBA16Snorm;
    case nrd::Format::RGBA16_SFLOAT: return Format::kRGBA16Float;
    case nrd::Format::R32_UINT: return Format::kR32Uint;
    case nrd::Format::R32_SFLOAT: return Format::kR32Float;
    case nrd::Format::RG32_SFLOAT: return Format::kRG32Float;
    case nrd::Format::RGBA32_SFLOAT: return Format::kRGBA32Float;
    case nrd::Format::R10_G10_B10_A2_UNORM: return Format::kRGB10A2Unorm;
    case nrd::Format::R11_G11_B10_UFLOAT: return Format::kRG11B10Float;
    case nrd::Format::R9_G9_B9_E5_UFLOAT: return Format::kRGB9E5Float;
    default: return Format::kUnknown;
  }
}

void CopyMatrix(float (&dst)[16], const Mat4& m) {
  std::memcpy(dst, m.m, sizeof(float) * 16);
}

}  // namespace

bool NrdDenoiser::Initialize(Device& device, Extent2D extent) {
  device_ = &device;
  VulkanHandles h = GetVulkanHandles(device);
  if (h.device == VK_NULL_HANDLE) {
    REC_WARN("nrd: requires the vulkan backend, denoiser unavailable");
    return false;
  }
  vk_device_ = h.device;

  const nrd::DenoiserDesc denoiser_descs[] = {
      {kAoIdentifier, nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION},
      {kShadowIdentifier, nrd::Denoiser::SIGMA_SHADOW},
      {kDiffuseIdentifier, nrd::Denoiser::REBLUR_DIFFUSE},
      {kSpecularIdentifier, nrd::Denoiser::REBLUR_SPECULAR},
  };
  nrd::InstanceCreationDesc creation{};
  creation.denoisers = denoiser_descs;
  creation.denoisersNum = static_cast<u32>(std::size(denoiser_descs));
  if (nrd::CreateInstance(creation, instance_) != nrd::Result::SUCCESS || !instance_) {
    REC_ERROR("nrd: instance creation failed");
    instance_ = nullptr;
    return false;
  }

  const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance_);
  resources_space_ = desc.resourcesSpaceIndex;
  const_samplers_space_ = desc.constantBufferAndSamplersSpaceIndex;
  constant_register_ = desc.constantBufferRegisterIndex;
  sampler_base_register_ = desc.samplersBaseRegisterIndex;
  resource_base_register_ = desc.resourcesBaseRegisterIndex;
  sampler_num_ = desc.samplersNum;

  if (!CreatePipelines(device) || !CreatePackPipeline(device) || !CreateDescriptorPools()) {
    Destroy(device);
    return false;
  }
  CreatePools(device, extent);

  const nrd::LibraryDesc& lib = *nrd::GetLibraryDesc();
  REC_INFO("nrd denoiser ready: v{}.{}.{}, {} pipelines, {} permanent + {} transient textures",
           lib.versionMajor, lib.versionMinor, lib.versionBuild, pipelines_.size(),
           permanent_.size(), transient_.size());
  return true;
}

bool NrdDenoiser::CreatePipelines(Device& device) {
  const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance_);
  const nrd::SPIRVBindingOffsets& off = nrd::GetLibraryDesc()->spirvBindingOffsets;

  // Samplers (NEAREST_CLAMP, LINEAR_CLAMP), immutable in the shared set.
  for (u32 i = 0; i < 2; ++i) {
    VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = i == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(vk_device_, &info, nullptr, &samplers_[i]) != VK_SUCCESS) return false;
  }

  // Shared set: one dynamic constant buffer + the immutable samplers.
  {
    base::Vector<VkDescriptorSetLayoutBinding> bindings;
    VkDescriptorSetLayoutBinding cb{};
    cb.binding = off.constantBufferOffset + constant_register_;
    cb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    cb.descriptorCount = 1;
    cb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings.push_back(cb);
    for (u32 i = 0; i < sampler_num_; ++i) {
      VkDescriptorSetLayoutBinding s{};
      s.binding = off.samplerOffset + sampler_base_register_ + i;
      s.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      s.descriptorCount = 1;
      s.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      s.pImmutableSamplers = &samplers_[i < 2 ? i : 1];
      bindings.push_back(s);
    }
    VkDescriptorSetLayoutCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<u32>(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(vk_device_, &info, nullptr, &const_set_layout_) !=
        VK_SUCCESS) {
      return false;
    }
  }

  pipelines_.resize(desc.pipelinesNum);
  for (u32 p = 0; p < desc.pipelinesNum; ++p) {
    const nrd::PipelineDesc& pd = desc.pipelines[p];
    Pipeline& out = pipelines_[p];
    out.has_constants = pd.hasConstantData;

    // Resource set: SAMPLED_IMAGE range (textures) + STORAGE_IMAGE range (UAVs).
    base::Vector<VkDescriptorSetLayoutBinding> bindings;
    for (u32 r = 0; r < pd.resourceRangesNum; ++r) {
      const nrd::ResourceRangeDesc& range = pd.resourceRanges[r];
      bool is_storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
      u32 base = is_storage ? off.storageTextureAndBufferOffset : off.textureOffset;
      base += resource_base_register_;
      for (u32 d = 0; d < range.descriptorsNum; ++d) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = base + d;
        b.descriptorType =
            is_storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings.push_back(b);
      }
      if (is_storage) {
        out.storage_num = range.descriptorsNum;
      } else {
        out.texture_num = range.descriptorsNum;
      }
    }
    VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = static_cast<u32>(bindings.size());
    set_info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(vk_device_, &set_info, nullptr, &out.resource_set_layout) !=
        VK_SUCCESS) {
      return false;
    }

    // Pipeline layout: set layouts ordered by space index.
    u32 max_space = resources_space_ > const_samplers_space_ ? resources_space_ : const_samplers_space_;
    base::Vector<VkDescriptorSetLayout> set_layouts(max_space + 1);  // count ctor zero-inits
    set_layouts[const_samplers_space_] = const_set_layout_;
    set_layouts[resources_space_] = out.resource_set_layout;
    VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = static_cast<u32>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    if (vkCreatePipelineLayout(vk_device_, &layout_info, nullptr, &out.layout) != VK_SUCCESS) {
      return false;
    }

    VkShaderModule module = CreateShaderModule(
        vk_device_, static_cast<const unsigned char*>(pd.computeShaderSPIRV.bytecode),
        pd.computeShaderSPIRV.size);
    if (module == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = desc.shaderEntryPoint;
    info.layout = out.layout;
    VkResult result =
        vkCreateComputePipelines(vk_device_, VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline);
    vkDestroyShaderModule(vk_device_, module, nullptr);
    if (result != VK_SUCCESS) return false;
  }

  // Constant buffer ring, double buffered by frame parity. Sized for the worst
  // case dispatch count across a frame.
  const nrd::InstanceDesc& d = desc;
  VkDeviceSize align = 256;  // safe minUniformBufferOffsetAlignment upper bound
  constant_slot_size_ = (d.constantBufferMaxDataSize + align - 1) & ~(align - 1);
  constant_slot_count_ = 64;
  constant_ring_ = device.CreateBuffer(constant_slot_size_ * constant_slot_count_ * 2,
                                       kBufferUsageUniform, true);
  return static_cast<bool>(constant_ring_);
}

bool NrdDenoiser::CreatePackPipeline(Device& device) {
  (void)device;
  // bindings: 0 = normal_roughness (UAV), 1 = viewZ (UAV), 2 = normals, 3 = depth.
  VkDescriptorSetLayoutBinding bindings[4]{};
  for (u32 i = 0; i < 4; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType =
        i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(vk_device_, &set_info, nullptr, &pack_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PackPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &pack_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(vk_device_, &layout_info, nullptr, &pack_layout_) != VK_SUCCESS) {
    return false;
  }
  VkShaderModule module =
      CreateShaderModule(vk_device_, k_nrd_pack_cs_hlsl, sizeof(k_nrd_pack_cs_hlsl));
  if (module == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = pack_layout_;
  VkResult result =
      vkCreateComputePipelines(vk_device_, VK_NULL_HANDLE, 1, &info, nullptr, &pack_pipeline_);
  vkDestroyShaderModule(vk_device_, module, nullptr);
  return result == VK_SUCCESS;
}

// The render graph's PassContext no longer hands out descriptor sets, so the
// denoiser owns its own pools: one per frame parity (matching the constant
// ring), reset in SetFrame before the frame's passes allocate from it. Sized
// for the worst case dispatch count the constant ring assumes (64), each
// dispatch with a resource set of up to 32 images, plus one shared
// constant+samplers set per denoise pass and the pack pass set.
bool NrdDenoiser::CreateDescriptorPools() {
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
  };
  VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.maxSets = 128;
  info.poolSizeCount = static_cast<u32>(sizeof(sizes) / sizeof(sizes[0]));
  info.pPoolSizes = sizes;
  for (VkDescriptorPool& pool : descriptor_pools_) {
    if (vkCreateDescriptorPool(vk_device_, &info, nullptr, &pool) != VK_SUCCESS) return false;
  }
  return true;
}

VkDescriptorSet NrdDenoiser::AllocateSet(VkDescriptorSetLayout layout) {
  VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  info.descriptorPool = descriptor_pools_[pool_parity_];
  info.descriptorSetCount = 1;
  info.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(vk_device_, &info, &set) != VK_SUCCESS) {
    REC_ERROR("nrd: descriptor set allocation failed (pool exhausted)");
    return VK_NULL_HANDLE;
  }
  return set;
}

NrdDenoiser::Inputs NrdDenoiser::PrepareInputs(RenderGraph& graph, ResourceHandle depth,
                                               ResourceHandle normals, f32 near_plane) {
  Inputs inputs;
  inputs.normal_roughness =
      graph.CreateTexture({.name = "nrd_normal_roughness", .format = kNormalRoughnessFormat,
                           .width = extent_.width, .height = extent_.height});
  inputs.view_z = graph.CreateTexture({.name = "nrd_viewz", .format = kViewZFormat,
                                       .width = extent_.width, .height = extent_.height});
  graph.AddPass(
      "nrd_pack",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Write(inputs.normal_roughness, ResourceUsage::kStorageWrite);
        builder.Write(inputs.view_z, ResourceUsage::kStorageWrite);
      },
      [this, depth, normals, inputs, near_plane](PassContext& ctx) {
        VkCommandBuffer vk_cmd = GetVkCommandBuffer(*ctx.cmd);
        VkDescriptorSet set = AllocateSet(pack_set_layout_);
        VkDescriptorImageInfo images[4]{};
        images[0] = {.imageView = GetVkImageView(ctx.graph->image(inputs.normal_roughness).view),
                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        images[1] = {.imageView = GetVkImageView(ctx.graph->image(inputs.view_z).view),
                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        images[2] = {.imageView = GetVkImageView(ctx.graph->image(normals).view),
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[3] = {.imageView = GetVkImageView(ctx.graph->image(depth).view),
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[4];
        for (u32 i = 0; i < 4; ++i) {
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType =
              i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(vk_device_, 4, writes, 0, nullptr);

        PackPush push{near_plane, kDenoisingRange, {0, 0}};
        vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack_pipeline_);
        vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack_layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(vk_cmd, pack_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(vk_cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
  return inputs;
}

void NrdDenoiser::CreatePools(Device& device, Extent2D extent) {
  extent_ = extent;
  const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance_);

  auto make_pool = [&](const nrd::TextureDesc* descs, u32 count, base::Vector<PoolTexture>& out) {
    out.resize(count);
    for (u32 i = 0; i < count; ++i) {
      Extent2D e{extent.width / (descs[i].downsampleFactor ? descs[i].downsampleFactor : 1),
                 extent.height / (descs[i].downsampleFactor ? descs[i].downsampleFactor : 1)};
      if (e.width == 0) e.width = 1;
      if (e.height == 0) e.height = 1;
      out[i].image = device.CreateImage2D(ToFormat(descs[i].format), e,
                                          kTextureUsageSampled | kTextureUsageStorage);
      out[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  };
  make_pool(desc.permanentPool, desc.permanentPoolSize, permanent_);
  make_pool(desc.transientPool, desc.transientPoolSize, transient_);

  auto make_output = [&](Format format) {
    PoolTexture t{};
    t.image = device.CreateImage2D(format, extent, kTextureUsageSampled | kTextureUsageStorage);
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return t;
  };
  out_ao_ = make_output(kHitDistFormat);
  out_shadow_ = make_output(kShadowFormat);
  out_diffuse_ = make_output(kDiffuseRadianceFormat);
  out_specular_ = make_output(kDiffuseRadianceFormat);
  out_ao_state_ = ResourceState::kGeneral;
  out_shadow_state_ = ResourceState::kGeneral;
  out_diffuse_state_ = ResourceState::kGeneral;
  out_specular_state_ = ResourceState::kGeneral;

  // Prime every owned texture into GENERAL so the first dispatch barrier has a
  // defined source layout.
  device.ImmediateSubmit([&](CommandList& cmd) {
    base::Vector<TextureBarrier> barriers;
    auto add = [&](PoolTexture& t) {
      barriers.push_back(Transition(t.image, ResourceState::kUndefined, ResourceState::kGeneral));
      t.layout = VK_IMAGE_LAYOUT_GENERAL;
    };
    for (auto& t : permanent_) add(t);
    for (auto& t : transient_) add(t);
    add(out_ao_);
    add(out_shadow_);
    add(out_diffuse_);
    add(out_specular_);
    cmd.TextureBarriers({barriers.data(), barriers.size()});
  });
}

void NrdDenoiser::DestroyPools(Device& device) {
  for (auto& t : permanent_) if (t.image) device.DestroyImage(t.image);
  for (auto& t : transient_) if (t.image) device.DestroyImage(t.image);
  permanent_.clear();
  transient_.clear();
  if (out_ao_.image) device.DestroyImage(out_ao_.image);
  if (out_shadow_.image) device.DestroyImage(out_shadow_.image);
  if (out_diffuse_.image) device.DestroyImage(out_diffuse_.image);
  if (out_specular_.image) device.DestroyImage(out_specular_.image);
}

void NrdDenoiser::Resize(Device& device, Extent2D extent) {
  DestroyPools(device);
  CreatePools(device, extent);
}

void NrdDenoiser::Destroy(Device& device) {
  DestroyPools(device);
  for (Pipeline& p : pipelines_) {
    if (p.pipeline) vkDestroyPipeline(vk_device_, p.pipeline, nullptr);
    if (p.layout) vkDestroyPipelineLayout(vk_device_, p.layout, nullptr);
    if (p.resource_set_layout)
      vkDestroyDescriptorSetLayout(vk_device_, p.resource_set_layout, nullptr);
  }
  pipelines_.clear();
  if (pack_pipeline_) vkDestroyPipeline(vk_device_, pack_pipeline_, nullptr);
  if (pack_layout_) vkDestroyPipelineLayout(vk_device_, pack_layout_, nullptr);
  if (pack_set_layout_) vkDestroyDescriptorSetLayout(vk_device_, pack_set_layout_, nullptr);
  pack_pipeline_ = VK_NULL_HANDLE;
  pack_layout_ = VK_NULL_HANDLE;
  pack_set_layout_ = VK_NULL_HANDLE;
  if (const_set_layout_) vkDestroyDescriptorSetLayout(vk_device_, const_set_layout_, nullptr);
  const_set_layout_ = VK_NULL_HANDLE;
  for (VkDescriptorPool& pool : descriptor_pools_) {
    if (pool) vkDestroyDescriptorPool(vk_device_, pool, nullptr);
    pool = VK_NULL_HANDLE;
  }
  for (VkSampler& s : samplers_) {
    if (s) vkDestroySampler(vk_device_, s, nullptr);
    s = VK_NULL_HANDLE;
  }
  if (constant_ring_) device.DestroyBuffer(constant_ring_);
  if (instance_) {
    nrd::DestroyInstance(*instance_);
    instance_ = nullptr;
  }
}

void NrdDenoiser::SetFrame(const FrameSettings& settings) {
  if (!instance_) return;
  nrd::CommonSettings common{};
  CopyMatrix(common.viewToClipMatrix, settings.view_to_clip);
  CopyMatrix(common.viewToClipMatrixPrev, settings.view_to_clip_prev);
  CopyMatrix(common.worldToViewMatrix, settings.world_to_view);
  CopyMatrix(common.worldToViewMatrixPrev, settings.world_to_view_prev);
  // The engine bakes pixel jitter into projection; NRD wants uv jitter in
  // [-0.5; 0.5] as "sampleUv = pixelUv + cameraJitter".
  common.cameraJitter[0] = settings.jitter[0] / static_cast<f32>(extent_.width);
  common.cameraJitter[1] = settings.jitter[1] / static_cast<f32>(extent_.height);
  common.cameraJitterPrev[0] = settings.jitter_prev[0] / static_cast<f32>(extent_.width);
  common.cameraJitterPrev[1] = settings.jitter_prev[1] / static_cast<f32>(extent_.height);
  // IN_MV is already a uv-space delta ((prev-curr)*0.5, same as the taa pass
  // samples), and NRD wants mv in uv too ("pixelUvPrev = pixelUv + mv", pixelUv
  // in 0..1), so the scale is identity. Scaling by the resolution made every
  // motion ~width times too large, so the slightest camera/geometry motion
  // reprojected off screen and nuked the whole shadow/ao history every frame.
  common.motionVectorScale[0] = 1.0f;
  common.motionVectorScale[1] = 1.0f;
  common.motionVectorScale[2] = 0.0f;
  common.resourceSize[0] = common.rectSize[0] = static_cast<uint16_t>(extent_.width);
  common.resourceSize[1] = common.rectSize[1] = static_cast<uint16_t>(extent_.height);
  common.resourceSizePrev[0] = common.rectSizePrev[0] = static_cast<uint16_t>(extent_.width);
  common.resourceSizePrev[1] = common.rectSizePrev[1] = static_cast<uint16_t>(extent_.height);
  common.frameIndex = settings.frame_index;
  common.accumulationMode =
      settings.reset ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
  common.isMotionVectorInWorldSpace = false;
  nrd::SetCommonSettings(*instance_, common);

  nrd::ReblurSettings reblur{};
  reblur.hitDistanceParameters.A = kHitDistParams[0];
  reblur.hitDistanceParameters.B = kHitDistParams[1];
  reblur.hitDistanceParameters.C = kHitDistParams[2];
  nrd::SetDenoiserSettings(*instance_, kAoIdentifier, &reblur);

  // The path-traced diffuse is a low-spp probabilistic signal over a scene with a
  // constantly-moving sun (day/night cycle) and a moving camera, so a long history
  // ghosts and lags badly. Make it responsive: shorten the main accumulation, and
  // most importantly cut the temporal-stabilization pass (an internal TAA that
  // defaults to a ~30-frame window and is the main source of the motion ghosting),
  // and make anti-lag more sensitive so it drops history faster on change. Grain
  // is handled by the multi-sampled input, not by over-accumulating.
  nrd::ReblurSettings diffuse = reblur;
  diffuse.maxAccumulatedFrameNum = settings.diffuse_accumulated_frames;  // tunable; default 16
  diffuse.maxStabilizedFrameNum = 4;                         // light stabilization, low ghost
  diffuse.antilagSettings.luminanceSensitivity = 2.0f;       // react faster to lighting change
  diffuse.historyFixFrameNum = 4;
  diffuse.enableAntiFirefly = true;
  nrd::SetDenoiserSettings(*instance_, kDiffuseIdentifier, &diffuse);

  // Specular reflections: 1-spp VNDF-sampled radiance. The lobe-driven
  // reprojection needs the real roughness guide; anti-firefly kills the
  // residual sky-hit sparkle on foliage.
  nrd::ReblurSettings specular = reblur;
  specular.maxAccumulatedFrameNum = 24;
  specular.maxStabilizedFrameNum = 8;
  specular.historyFixFrameNum = 4;
  specular.enableAntiFirefly = true;
  nrd::SetDenoiserSettings(*instance_, kSpecularIdentifier, &specular);

  nrd::SigmaSettings sigma{};
  sigma.lightDirection[0] = -settings.sun_direction.x;
  sigma.lightDirection[1] = -settings.sun_direction.y;
  sigma.lightDirection[2] = -settings.sun_direction.z;
  nrd::SetDenoiserSettings(*instance_, kShadowIdentifier, &sigma);

  constant_cursor_ = (settings.frame_index & 1u) * constant_slot_count_;
  // Descriptor sets follow the same frame parity as the constant ring: the
  // other parity's sets may still be in flight, this parity's were consumed
  // two frames ago. SetFrame runs at graph-build time, before any of this
  // frame's passes execute (and allocate).
  pool_parity_ = settings.frame_index & 1u;
  vkResetDescriptorPool(vk_device_, descriptor_pools_[pool_parity_], 0);
}

ResourceHandle NrdDenoiser::DenoiseAo(RenderGraph& graph, ResourceHandle normal_roughness,
                                      ResourceHandle view_z, ResourceHandle motion,
                                      ResourceHandle in_hitdist) {
  return AddDenoisePass(graph, kAoIdentifier, "nrd_ao", normal_roughness, view_z, motion, in_hitdist,
                        static_cast<int>(nrd::ResourceType::IN_DIFF_HITDIST), out_ao_,
                        static_cast<int>(nrd::ResourceType::OUT_DIFF_HITDIST), "nrd_ao_out",
                        &out_ao_state_);
}

ResourceHandle NrdDenoiser::DenoiseShadow(RenderGraph& graph, ResourceHandle normal_roughness,
                                          ResourceHandle view_z, ResourceHandle motion,
                                          ResourceHandle in_penumbra) {
  return AddDenoisePass(graph, kShadowIdentifier, "nrd_shadow", normal_roughness, view_z, motion,
                        in_penumbra, static_cast<int>(nrd::ResourceType::IN_PENUMBRA), out_shadow_,
                        static_cast<int>(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY),
                        "nrd_shadow_out", &out_shadow_state_);
}

ResourceHandle NrdDenoiser::DenoiseDiffuse(RenderGraph& graph, ResourceHandle normal_roughness,
                                           ResourceHandle view_z, ResourceHandle motion,
                                           ResourceHandle in_radiance_hitdist) {
  return AddDenoisePass(graph, kDiffuseIdentifier, "nrd_diffuse", normal_roughness, view_z, motion,
                        in_radiance_hitdist,
                        static_cast<int>(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST), out_diffuse_,
                        static_cast<int>(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST),
                        "nrd_diffuse_out", &out_diffuse_state_);
}

ResourceHandle NrdDenoiser::DenoiseSpecular(RenderGraph& graph, ResourceHandle normal_roughness,
                                            ResourceHandle view_z, ResourceHandle motion,
                                            ResourceHandle in_radiance_hitdist) {
  return AddDenoisePass(graph, kSpecularIdentifier, "nrd_specular", normal_roughness, view_z,
                        motion, in_radiance_hitdist,
                        static_cast<int>(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST),
                        out_specular_,
                        static_cast<int>(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST),
                        "nrd_specular_out", &out_specular_state_);
}

ResourceHandle NrdDenoiser::AddDenoisePass(RenderGraph& graph, u32 identifier, const char* pass_name,
                                           ResourceHandle normal_roughness, ResourceHandle view_z,
                                           ResourceHandle motion, ResourceHandle noisy,
                                           int noisy_type, PoolTexture& output, int output_type,
                                           const char* output_name, ResourceState* output_state) {
  ResourceHandle out_handle = graph.ImportImage(output_name, output.image, output_state);
  graph.AddPass(
      pass_name,
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(normal_roughness, ResourceUsage::kSampledCompute);
        builder.Read(view_z, ResourceUsage::kSampledCompute);
        builder.Read(motion, ResourceUsage::kSampledCompute);
        builder.Read(noisy, ResourceUsage::kSampledCompute);
        builder.Write(out_handle, ResourceUsage::kStorageWrite);
      },
      [this, identifier, normal_roughness, view_z, motion, noisy, noisy_type, &output, output_type](
          PassContext& ctx) {
        for (auto& b : bindings_) b = {};
        bindings_[static_cast<int>(nrd::ResourceType::IN_MV)] = {
            GetVkImageView(ctx.graph->image(motion).view), nullptr};
        bindings_[static_cast<int>(nrd::ResourceType::IN_NORMAL_ROUGHNESS)] = {
            GetVkImageView(ctx.graph->image(normal_roughness).view), nullptr};
        bindings_[static_cast<int>(nrd::ResourceType::IN_VIEWZ)] = {
            GetVkImageView(ctx.graph->image(view_z).view), nullptr};
        bindings_[noisy_type] = {GetVkImageView(ctx.graph->image(noisy).view), nullptr};
        // The graph leaves the imported output in GENERAL before the pass.
        output.layout = VK_IMAGE_LAYOUT_GENERAL;
        bindings_[output_type] = {GetVkImageView(output.image.view), &output};
        RecordDispatches(ctx, identifier);
        // Hand the output back to the graph in GENERAL (its kStorageWrite state).
        if (output.layout != VK_IMAGE_LAYOUT_GENERAL) {
          VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
          b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
          b.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
          b.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
          b.oldLayout = output.layout;
          b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          b.image = GetVkImage(output.image);
          b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
          dep.imageMemoryBarrierCount = 1;
          dep.pImageMemoryBarriers = &b;
          vkCmdPipelineBarrier2(GetVkCommandBuffer(*ctx.cmd), &dep);
          output.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
      });
  return out_handle;
}

void NrdDenoiser::RecordDispatches(PassContext& ctx, u32 identifier) {
  const nrd::DispatchDesc* dispatches = nullptr;
  uint32_t dispatch_num = 0;
  nrd::Result res = nrd::GetComputeDispatches(*instance_, &identifier, 1, dispatches, dispatch_num);
  if (res != nrd::Result::SUCCESS) {
    REC_WARN("nrd: GetComputeDispatches({}) failed ({})", identifier, static_cast<int>(res));
    return;
  }
  if (dispatch_num == 0) REC_WARN("nrd: identifier {} produced 0 dispatches", identifier);

  const nrd::SPIRVBindingOffsets& off = nrd::GetLibraryDesc()->spirvBindingOffsets;
  VkCommandBuffer vk_cmd = GetVkCommandBuffer(*ctx.cmd);
  VkDevice dev = vk_device_;

  // One shared (constant + samplers) set for the whole denoiser, bound with a
  // per-dispatch dynamic offset into the constant ring.
  VkDescriptorSet const_set = AllocateSet(const_set_layout_);
  VkDescriptorBufferInfo cb_info{GetVkBuffer(constant_ring_), 0, constant_slot_size_};
  VkWriteDescriptorSet cb_write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  cb_write.dstSet = const_set;
  cb_write.dstBinding = off.constantBufferOffset + constant_register_;
  cb_write.descriptorCount = 1;
  cb_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  cb_write.pBufferInfo = &cb_info;
  vkUpdateDescriptorSets(dev, 1, &cb_write, 0, nullptr);

  for (uint32_t i = 0; i < dispatch_num; ++i) {
    const nrd::DispatchDesc& d = dispatches[i];
    const Pipeline& pipe = pipelines_[d.pipelineIndex];

    // Constant data into the ring.
    u32 dynamic_offset = 0;
    if (d.constantBufferDataSize) {
      if (constant_cursor_ >= (constant_slot_count_ * 2)) {
        constant_cursor_ = 0;  // overflow guard; should not happen
      }
      dynamic_offset = static_cast<u32>(constant_cursor_ * constant_slot_size_);
      std::memcpy(static_cast<u8*>(constant_ring_.mapped) + dynamic_offset, d.constantBufferData,
                  d.constantBufferDataSize);
      constant_cursor_++;
    }

    // Resolve resources, emit transition barriers and descriptor writes.
    VkDescriptorSet res_set = AllocateSet(pipe.resource_set_layout);
    VkDescriptorImageInfo image_infos[32]{};
    VkWriteDescriptorSet writes[32];
    VkImageMemoryBarrier2 barriers[32];
    u32 write_count = 0, barrier_count = 0, tex_index = 0, storage_index = 0;

    for (u32 r = 0; r < d.resourcesNum; ++r) {
      const nrd::ResourceDesc& res = d.resources[r];
      bool is_storage = res.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;

      VkImageView view = VK_NULL_HANDLE;
      PoolTexture* tracked = nullptr;
      if (res.type == nrd::ResourceType::TRANSIENT_POOL) {
        tracked = &transient_[res.indexInPool];
        view = GetVkImageView(tracked->image.view);
      } else if (res.type == nrd::ResourceType::PERMANENT_POOL) {
        tracked = &permanent_[res.indexInPool];
        view = GetVkImageView(tracked->image.view);
      } else {
        const ResourceBinding& b = bindings_[static_cast<int>(res.type)];
        view = b.view;
        tracked = b.tracked;
      }

      VkImageLayout want =
          is_storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (tracked && tracked->layout != want) {
        VkImageMemoryBarrier2& b = barriers[barrier_count++];
        b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstAccessMask = is_storage ? VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
                                     : VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = tracked->layout;
        b.newLayout = want;
        b.image = GetVkImage(tracked->image);
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        tracked->layout = want;
      } else if (tracked && is_storage) {
        // UAV reused as UAV: order the write-after-write/read.
        VkImageMemoryBarrier2& b = barriers[barrier_count++];
        b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = GetVkImage(tracked->image);
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }

      u32 binding = is_storage ? off.storageTextureAndBufferOffset + resource_base_register_ +
                                     (storage_index++)
                               : off.textureOffset + resource_base_register_ + (tex_index++);
      VkDescriptorImageInfo& info = image_infos[write_count];
      info.imageView = view;
      info.imageLayout = want;
      VkWriteDescriptorSet& w = writes[write_count];
      w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = res_set;
      w.dstBinding = binding;
      w.descriptorCount = 1;
      w.descriptorType =
          is_storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w.pImageInfo = &info;
      write_count++;
    }

    if (barrier_count) {
      VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = barrier_count;
      dep.pImageMemoryBarriers = barriers;
      vkCmdPipelineBarrier2(vk_cmd, &dep);
    }
    if (write_count) vkUpdateDescriptorSets(dev, write_count, writes, 0, nullptr);

    vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout,
                            const_samplers_space_, 1, &const_set, 1, &dynamic_offset);
    vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout, resources_space_,
                            1, &res_set, 0, nullptr);
    vkCmdDispatch(vk_cmd, d.gridWidth, d.gridHeight, 1);
  }
}

}  // namespace rec::render

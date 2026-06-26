#include "render/gi/denoiser_nrd.h"

#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/nrd_pack_cs_hlsl.h"

#include <NRD.h>

namespace rec::render {
namespace {

constexpr u32 kAoIdentifier = 0;
constexpr u32 kShadowIdentifier = 1;
// Sky / invalid pixels report a viewZ beyond this so NRD ignores them.
constexpr f32 kDenoisingRange = 1.0e6f;

struct PackPush {
  f32 near_plane;
  f32 denoising_range;
  f32 pad[2];
};

VkFormat ToVkFormat(nrd::Format format) {
  switch (format) {
    case nrd::Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM: return VK_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT: return VK_FORMAT_R8_UINT;
    case nrd::Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case nrd::Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::R16_UNORM: return VK_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM: return VK_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT: return VK_FORMAT_R16_UINT;
    case nrd::Format::R16_SFLOAT: return VK_FORMAT_R16_SFLOAT;
    case nrd::Format::RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_SFLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case nrd::Format::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case nrd::Format::R32_UINT: return VK_FORMAT_R32_UINT;
    case nrd::Format::R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
    case nrd::Format::RG32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case nrd::Format::RGBA32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case nrd::Format::R11_G11_B10_UFLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case nrd::Format::R9_G9_B9_E5_UFLOAT: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
  }
}

void CopyMatrix(float (&dst)[16], const Mat4& m) {
  std::memcpy(dst, m.m, sizeof(float) * 16);
}

}  // namespace

bool NrdDenoiser::Initialize(Device& device, VkExtent2D extent) {
  device_ = &device;

  const nrd::DenoiserDesc denoiser_descs[] = {
      {kAoIdentifier, nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION},
      {kShadowIdentifier, nrd::Denoiser::SIGMA_SHADOW},
  };
  nrd::InstanceCreationDesc creation{};
  creation.denoisers = denoiser_descs;
  creation.denoisersNum = 2;
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

  if (!CreatePipelines(device) || !CreatePackPipeline(device)) {
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
    if (vkCreateSampler(device.device(), &info, nullptr, &samplers_[i]) != VK_SUCCESS) return false;
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
    if (vkCreateDescriptorSetLayout(device.device(), &info, nullptr, &const_set_layout_) !=
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
    if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &out.resource_set_layout) !=
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
    if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &out.layout) != VK_SUCCESS) {
      return false;
    }

    VkShaderModule module = CreateShaderModule(
        device.device(), static_cast<const unsigned char*>(pd.computeShaderSPIRV.bytecode),
        pd.computeShaderSPIRV.size);
    if (module == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = desc.shaderEntryPoint;
    info.layout = out.layout;
    VkResult result =
        vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline);
    vkDestroyShaderModule(device.device(), module, nullptr);
    if (result != VK_SUCCESS) return false;
  }

  // Constant buffer ring, double buffered by frame parity. Sized for the worst
  // case dispatch count across a frame.
  const nrd::InstanceDesc& d = desc;
  VkDeviceSize align = 256;  // safe minUniformBufferOffsetAlignment upper bound
  constant_slot_size_ = (d.constantBufferMaxDataSize + align - 1) & ~(align - 1);
  constant_slot_count_ = 64;
  constant_ring_ = device.CreateBuffer(constant_slot_size_ * constant_slot_count_ * 2,
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
  return constant_ring_.buffer != VK_NULL_HANDLE;
}

bool NrdDenoiser::CreatePackPipeline(Device& device) {
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
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &pack_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PackPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &pack_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &pack_layout_) != VK_SUCCESS) {
    return false;
  }
  VkShaderModule module =
      CreateShaderModule(device.device(), k_nrd_pack_cs_hlsl, sizeof(k_nrd_pack_cs_hlsl));
  if (module == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = pack_layout_;
  VkResult result =
      vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pack_pipeline_);
  vkDestroyShaderModule(device.device(), module, nullptr);
  return result == VK_SUCCESS;
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
        VkDescriptorSet set = ctx.allocate_set(pack_set_layout_);
        VkDescriptorImageInfo images[4]{};
        images[0] = {.imageView = ctx.graph->image(inputs.normal_roughness).view,
                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        images[1] = {.imageView = ctx.graph->image(inputs.view_z).view,
                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        images[2] = {.imageView = ctx.graph->image(normals).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[3] = {.imageView = ctx.graph->image(depth).view,
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
        vkUpdateDescriptorSets(ctx.device->device(), 4, writes, 0, nullptr);

        PackPush push{near_plane, kDenoisingRange, {0, 0}};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack_layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, pack_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
  return inputs;
}

void NrdDenoiser::CreatePools(Device& device, VkExtent2D extent) {
  extent_ = extent;
  const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance_);

  auto make_pool = [&](const nrd::TextureDesc* descs, u32 count, base::Vector<PoolTexture>& out) {
    out.resize(count);
    for (u32 i = 0; i < count; ++i) {
      VkExtent2D e{extent.width / (descs[i].downsampleFactor ? descs[i].downsampleFactor : 1),
                   extent.height / (descs[i].downsampleFactor ? descs[i].downsampleFactor : 1)};
      if (e.width == 0) e.width = 1;
      if (e.height == 0) e.height = 1;
      out[i].image = device.CreateImage2D(ToVkFormat(descs[i].format), e,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_IMAGE_ASPECT_COLOR_BIT);
      out[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  };
  make_pool(desc.permanentPool, desc.permanentPoolSize, permanent_);
  make_pool(desc.transientPool, desc.transientPoolSize, transient_);

  auto make_output = [&](VkFormat format) {
    PoolTexture t{};
    t.image = device.CreateImage2D(format, extent,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return t;
  };
  out_ao_ = make_output(kHitDistFormat);
  out_shadow_ = make_output(kShadowFormat);
  out_ao_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  out_shadow_layout_ = VK_IMAGE_LAYOUT_GENERAL;

  // Prime every owned texture into GENERAL so the first dispatch barrier has a
  // defined source layout.
  device.ImmediateSubmit([&](VkCommandBuffer cmd) {
    base::Vector<VkImageMemoryBarrier2> barriers;
    auto add = [&](VkImage image) {
      VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.image = image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barriers.push_back(b);
    };
    for (auto& t : permanent_) { add(t.image.image); t.layout = VK_IMAGE_LAYOUT_GENERAL; }
    for (auto& t : transient_) { add(t.image.image); t.layout = VK_IMAGE_LAYOUT_GENERAL; }
    add(out_ao_.image.image);
    add(out_shadow_.image.image);
    out_ao_.layout = out_shadow_.layout = VK_IMAGE_LAYOUT_GENERAL;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<u32>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
  });
}

void NrdDenoiser::DestroyPools(Device& device) {
  for (auto& t : permanent_) if (t.image.image) device.DestroyImage(t.image);
  for (auto& t : transient_) if (t.image.image) device.DestroyImage(t.image);
  permanent_.clear();
  transient_.clear();
  if (out_ao_.image.image) device.DestroyImage(out_ao_.image);
  if (out_shadow_.image.image) device.DestroyImage(out_shadow_.image);
}

void NrdDenoiser::Resize(Device& device, VkExtent2D extent) {
  DestroyPools(device);
  CreatePools(device, extent);
}

void NrdDenoiser::Destroy(Device& device) {
  DestroyPools(device);
  for (Pipeline& p : pipelines_) {
    if (p.pipeline) vkDestroyPipeline(device.device(), p.pipeline, nullptr);
    if (p.layout) vkDestroyPipelineLayout(device.device(), p.layout, nullptr);
    if (p.resource_set_layout)
      vkDestroyDescriptorSetLayout(device.device(), p.resource_set_layout, nullptr);
  }
  pipelines_.clear();
  if (pack_pipeline_) vkDestroyPipeline(device.device(), pack_pipeline_, nullptr);
  if (pack_layout_) vkDestroyPipelineLayout(device.device(), pack_layout_, nullptr);
  if (pack_set_layout_) vkDestroyDescriptorSetLayout(device.device(), pack_set_layout_, nullptr);
  pack_pipeline_ = VK_NULL_HANDLE;
  pack_layout_ = VK_NULL_HANDLE;
  pack_set_layout_ = VK_NULL_HANDLE;
  if (const_set_layout_) vkDestroyDescriptorSetLayout(device.device(), const_set_layout_, nullptr);
  const_set_layout_ = VK_NULL_HANDLE;
  for (VkSampler& s : samplers_) {
    if (s) vkDestroySampler(device.device(), s, nullptr);
    s = VK_NULL_HANDLE;
  }
  if (constant_ring_.buffer) device.DestroyBuffer(constant_ring_);
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

  nrd::SigmaSettings sigma{};
  sigma.lightDirection[0] = -settings.sun_direction.x;
  sigma.lightDirection[1] = -settings.sun_direction.y;
  sigma.lightDirection[2] = -settings.sun_direction.z;
  nrd::SetDenoiserSettings(*instance_, kShadowIdentifier, &sigma);

  constant_cursor_ = (settings.frame_index & 1u) * constant_slot_count_;
}

ResourceHandle NrdDenoiser::DenoiseAo(RenderGraph& graph, ResourceHandle normal_roughness,
                                      ResourceHandle view_z, ResourceHandle motion,
                                      ResourceHandle in_hitdist) {
  return AddDenoisePass(graph, kAoIdentifier, "nrd_ao", normal_roughness, view_z, motion, in_hitdist,
                        static_cast<int>(nrd::ResourceType::IN_DIFF_HITDIST), out_ao_,
                        static_cast<int>(nrd::ResourceType::OUT_DIFF_HITDIST), "nrd_ao_out",
                        &out_ao_layout_);
}

ResourceHandle NrdDenoiser::DenoiseShadow(RenderGraph& graph, ResourceHandle normal_roughness,
                                          ResourceHandle view_z, ResourceHandle motion,
                                          ResourceHandle in_penumbra) {
  return AddDenoisePass(graph, kShadowIdentifier, "nrd_shadow", normal_roughness, view_z, motion,
                        in_penumbra, static_cast<int>(nrd::ResourceType::IN_PENUMBRA), out_shadow_,
                        static_cast<int>(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY),
                        "nrd_shadow_out", &out_shadow_layout_);
}

ResourceHandle NrdDenoiser::AddDenoisePass(RenderGraph& graph, u32 identifier, const char* pass_name,
                                           ResourceHandle normal_roughness, ResourceHandle view_z,
                                           ResourceHandle motion, ResourceHandle noisy,
                                           int noisy_type, PoolTexture& output, int output_type,
                                           const char* output_name, VkImageLayout* output_layout) {
  ResourceHandle out_handle = graph.ImportImage(output_name, output.image, output_layout);
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
        bindings_[static_cast<int>(nrd::ResourceType::IN_MV)] = {ctx.graph->image(motion).view,
                                                                 nullptr};
        bindings_[static_cast<int>(nrd::ResourceType::IN_NORMAL_ROUGHNESS)] = {
            ctx.graph->image(normal_roughness).view, nullptr};
        bindings_[static_cast<int>(nrd::ResourceType::IN_VIEWZ)] = {ctx.graph->image(view_z).view,
                                                                    nullptr};
        bindings_[noisy_type] = {ctx.graph->image(noisy).view, nullptr};
        // The graph leaves the imported output in GENERAL before the pass.
        output.layout = VK_IMAGE_LAYOUT_GENERAL;
        bindings_[output_type] = {output.image.view, &output};
        RecordDispatches(ctx, identifier);
        // Hand the output back to the graph in GENERAL (its kStorageWrite state).
        if (output.layout != VK_IMAGE_LAYOUT_GENERAL) {
          VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
          b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
          b.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
          b.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
          b.oldLayout = output.layout;
          b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          b.image = output.image.image;
          b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
          dep.imageMemoryBarrierCount = 1;
          dep.pImageMemoryBarriers = &b;
          vkCmdPipelineBarrier2(ctx.cmd, &dep);
          output.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
      });
  return out_handle;
}

void NrdDenoiser::RecordDispatches(PassContext& ctx, u32 identifier) {
  const nrd::DispatchDesc* dispatches = nullptr;
  uint32_t dispatch_num = 0;
  if (nrd::GetComputeDispatches(*instance_, &identifier, 1, dispatches, dispatch_num) !=
      nrd::Result::SUCCESS) {
    return;
  }

  const nrd::SPIRVBindingOffsets& off = nrd::GetLibraryDesc()->spirvBindingOffsets;
  VkDevice dev = ctx.device->device();

  // One shared (constant + samplers) set for the whole denoiser, bound with a
  // per-dispatch dynamic offset into the constant ring.
  VkDescriptorSet const_set = ctx.allocate_set(const_set_layout_);
  VkDescriptorBufferInfo cb_info{constant_ring_.buffer, 0, constant_slot_size_};
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
    VkDescriptorSet res_set = ctx.allocate_set(pipe.resource_set_layout);
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
        view = tracked->image.view;
      } else if (res.type == nrd::ResourceType::PERMANENT_POOL) {
        tracked = &permanent_[res.indexInPool];
        view = tracked->image.view;
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
        b.image = tracked->image.image;
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
        b.image = tracked->image.image;
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
      vkCmdPipelineBarrier2(ctx.cmd, &dep);
    }
    if (write_count) vkUpdateDescriptorSets(dev, write_count, writes, 0, nullptr);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout,
                            const_samplers_space_, 1, &const_set, 1, &dynamic_offset);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout, resources_space_,
                            1, &res_set, 0, nullptr);
    vkCmdDispatch(ctx.cmd, d.gridWidth, d.gridHeight, 1);
  }
}

}  // namespace rec::render

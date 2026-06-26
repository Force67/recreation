#include "render/gi/ddgi.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/util/shader_util.h"
#include "shaders/ddgi_blend_cs_hlsl.h"
#include "shaders/ddgi_border_cs_hlsl.h"
#include "shaders/ddgi_rays_cs_hlsl.h"

namespace rec::render {
namespace {

constexpr u32 kProbeCount = DdgiSystem::kProbesX * DdgiSystem::kProbesY * DdgiSystem::kProbesZ;
constexpr u32 kIrradianceWidth =
    (DdgiSystem::kIrradianceTexels + 2) * DdgiSystem::kProbesX * DdgiSystem::kProbesZ;
constexpr u32 kIrradianceHeight = (DdgiSystem::kIrradianceTexels + 2) * DdgiSystem::kProbesY;
constexpr u32 kDistanceWidth =
    (DdgiSystem::kDistanceTexels + 2) * DdgiSystem::kProbesX * DdgiSystem::kProbesZ;
constexpr u32 kDistanceHeight = (DdgiSystem::kDistanceTexels + 2) * DdgiSystem::kProbesY;

struct RaysPush {
  f32 rotation[12];  // three float4 rows
  f32 sun_direction[4];
  f32 sun_color[4];
};

struct BlendPush {
  f32 rotation[12];
  u32 mode;
  u32 ray_count;
  u32 reset;
  f32 pad;
};

struct BorderPush {
  u32 texels;
  u32 probes_x;
  u32 probes_y;
  u32 pad;
};

// Uniformly random rotation per frame so the fibonacci sphere covers all
// directions over time. Axis-angle from a weyl-sequence hash.
void FrameRotation(u32 frame_index, f32 out_rows[12]) {
  auto hash = [](u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
  };
  f32 u1 = static_cast<f32>(hash(frame_index) & 0xffffff) / 16777215.0f;
  f32 u2 = static_cast<f32>(hash(frame_index + 1) & 0xffffff) / 16777215.0f;
  f32 u3 = static_cast<f32>(hash(frame_index + 2) & 0xffffff) / 16777215.0f;
  f32 angle = u1 * 6.2831853f;
  f32 z = u2 * 2.0f - 1.0f;
  f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  f32 phi = u3 * 6.2831853f;
  Vec3 axis{r * std::cos(phi), r * std::sin(phi), z};

  f32 c = std::cos(angle);
  f32 s = std::sin(angle);
  f32 t = 1.0f - c;
  f32 rows[12] = {
      t * axis.x * axis.x + c,          t * axis.x * axis.y - s * axis.z,
      t * axis.x * axis.z + s * axis.y, 0,
      t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c,
      t * axis.y * axis.z - s * axis.x, 0,
      t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x,
      t * axis.z * axis.z + c,          0,
  };
  std::memcpy(out_rows, rows, sizeof(rows));
}

void MemoryBarrier2(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access) {
  VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

std::unique_ptr<DdgiSystem> DdgiSystem::Create(Device& device, VkImageView sky_view,
                                               VkSampler sky_sampler,
                                               BindlessRegistry& bindless) {
  auto ddgi = std::unique_ptr<DdgiSystem>(new DdgiSystem(device));
  ddgi->sky_view_ = sky_view;
  ddgi->sky_sampler_ = sky_sampler;
  ddgi->bindless_ = &bindless;
  if (!ddgi->CreateResources(sky_view, sky_sampler) || !ddgi->CreatePipelines()) return nullptr;
  return ddgi;
}

void DdgiSystem::Configure(const Settings& settings) {
  if (settings.probe_spacing != settings_.probe_spacing) history_valid_ = false;
  settings_ = settings;
}

bool DdgiSystem::CreateResources(VkImageView sky_view, VkSampler sky_sampler) {
  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_.device(), &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }

  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  irradiance_ = device_.CreateImage2D(VK_FORMAT_R16G16B16A16_SFLOAT,
                                      {kIrradianceWidth, kIrradianceHeight}, usage,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
  distance_ = device_.CreateImage2D(VK_FORMAT_R16G16B16A16_SFLOAT,
                                    {kDistanceWidth, kDistanceHeight}, usage,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
  rays_ = device_.CreateImage2D(VK_FORMAT_R16G16B16A16_SFLOAT, {kRaysPerProbe, kProbeCount},
                                usage, VK_IMAGE_ASPECT_COLOR_BIT);
  if (!irradiance_.image || !distance_.image || !rays_.image) return false;

  auto array_view = [&](VkImage image, VkImageView* out) {
    VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device_.device(), &info, nullptr, out) == VK_SUCCESS;
  };
  if (!array_view(irradiance_.image, &irradiance_array_view_) ||
      !array_view(distance_.image, &distance_array_view_)) {
    return false;
  }

  for (GpuBuffer& buffer : volume_buffers_) {
    buffer = device_.CreateBuffer(sizeof(VolumeData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    if (!buffer.mapped) return false;
  }
  return true;
}

bool DdgiSystem::CreatePipelines() {
  auto make = [&](const unsigned char* spv, size_t spv_size, u32 sampled, bool tlas, bool volume,
                  u32 push_size, VkDescriptorSetLayout* set_layout, VkPipelineLayout* layout,
                  VkPipeline* pipeline) {
    VkDescriptorSetLayoutBinding bindings[5]{};
    u32 count = 0;
    bindings[count] = {.binding = count, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    ++count;
    for (u32 i = 0; i < sampled; ++i, ++count) {
      bindings[count] = {.binding = count,
                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    if (tlas) {
      bindings[count] = {.binding = count,
                         .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      ++count;
    }
    if (volume) {
      bindings[count] = {.binding = count, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      ++count;
    }
    VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = count;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_.device(), &set_info, nullptr, set_layout) !=
        VK_SUCCESS) {
      return false;
    }
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkDescriptorSetLayout set_layouts[2] = {*set_layout, bindless_->set_layout()};
    VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    // The rays pass resolves hit materials through the bindless set.
    layout_info.setLayoutCount = tlas ? 2 : 1;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_.device(), &layout_info, nullptr, layout) != VK_SUCCESS) {
      return false;
    }
    VkShaderModule module = CreateShaderModule(device_.device(), spv, spv_size);
    if (module == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = *layout;
    VkResult result =
        vkCreateComputePipelines(device_.device(), VK_NULL_HANDLE, 1, &info, nullptr, pipeline);
    vkDestroyShaderModule(device_.device(), module, nullptr);
    return result == VK_SUCCESS;
  };

  if (!make(k_ddgi_rays_cs_hlsl, sizeof(k_ddgi_rays_cs_hlsl), 2, true, true, sizeof(RaysPush),
            &rays_set_layout_, &rays_layout_, &rays_pipeline_) ||
      !make(k_ddgi_blend_cs_hlsl, sizeof(k_ddgi_blend_cs_hlsl), 1, false, true,
            sizeof(BlendPush), &blend_set_layout_, &blend_layout_, &blend_pipeline_) ||
      !make(k_ddgi_border_cs_hlsl, sizeof(k_ddgi_border_cs_hlsl), 0, false, false,
            sizeof(BorderPush), &border_set_layout_, &border_layout_, &border_pipeline_)) {
    REC_ERROR("ddgi pipeline creation failed");
    return false;
  }
  return true;
}

EnvironmentSystem::DdgiBinding DdgiSystem::binding(u32 frame_index) const {
  EnvironmentSystem::DdgiBinding result;
  result.irradiance = irradiance_array_view_;
  result.distance = distance_array_view_;
  result.volume = volume_buffers_[frame_index % 2].buffer;
  result.volume_size = sizeof(VolumeData);
  result.layout = VK_IMAGE_LAYOUT_GENERAL;  // atlases live in GENERAL
  return result;
}

void DdgiSystem::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            const Vec3& camera, const Vec3& sun_direction, f32 sun_intensity,
                            const Vec3& sun_color, u32 frame_index) {
  // Snap the volume to the probe grid around the camera; a snap shifts what
  // every probe represents, so history resets and re-converges.
  f32 spacing = settings_.probe_spacing;
  Vec3 extent{(kProbesX - 1) * spacing, (kProbesY - 1) * spacing, (kProbesZ - 1) * spacing};
  Vec3 origin{std::floor((camera.x - extent.x * 0.5f) / spacing) * spacing,
              std::floor((camera.y - extent.y * 0.5f) / spacing) * spacing,
              std::floor((camera.z - extent.z * 0.5f) / spacing) * spacing};
  bool snapped = origin.x != origin_.x || origin.y != origin_.y || origin.z != origin_.z;
  origin_ = origin;
  bool reset = !history_valid_ || snapped;
  history_valid_ = true;

  VolumeData volume{};
  volume.origin[0] = origin.x;
  volume.origin[1] = origin.y;
  volume.origin[2] = origin.z;
  volume.origin[3] = spacing;
  volume.counts[0] = kProbesX;
  volume.counts[1] = kProbesY;
  volume.counts[2] = kProbesZ;
  volume.counts[3] = kIrradianceTexels;
  volume.params[0] = static_cast<f32>(kDistanceTexels);
  volume.params[1] = settings_.hysteresis;
  volume.params[2] = spacing * 4.0f;  // max ray distance
  volume.params[3] = settings_.energy_scale;
  GpuBuffer& volume_buffer = volume_buffers_[frame_index % 2];
  std::memcpy(volume_buffer.mapped, &volume, sizeof(volume));

  RaysPush rays_push{};
  FrameRotation(frame_index, rays_push.rotation);
  Vec3 sun = Normalize(sun_direction);
  rays_push.sun_direction[0] = sun.x;
  rays_push.sun_direction[1] = sun.y;
  rays_push.sun_direction[2] = sun.z;
  rays_push.sun_direction[3] = sun_intensity;
  rays_push.sun_color[0] = sun_color.x;
  rays_push.sun_color[1] = sun_color.y;
  rays_push.sun_color[2] = sun_color.z;
  rays_push.sun_color[3] = static_cast<f32>(kRaysPerProbe);

  graph.AddPass(
      "ddgi", [](RenderGraph::PassBuilder&) {},
      [this, &raytracing, tlas_slot, rays_push, frame_index, reset](PassContext& ctx) {
        VkBuffer volume_buffer = volume_buffers_[frame_index % 2].buffer;

        // Everything stays in GENERAL; first touch transitions from
        // UNDEFINED, after that plain memory barriers order the stages.
        if (!atlas_initialized_) {
          atlas_initialized_ = true;
          VkImageMemoryBarrier2 barriers[3];
          VkImage images[3] = {irradiance_.image, distance_.image, rays_.image};
          for (u32 i = 0; i < 3; ++i) {
            barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barriers[i].dstAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].image = images[i];
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          }
          VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
          dep.imageMemoryBarrierCount = 3;
          dep.pImageMemoryBarriers = barriers;
          vkCmdPipelineBarrier2(ctx.cmd, &dep);
        } else {
          // Last frame's fragment reads must finish before we rewrite.
          MemoryBarrier2(ctx.cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }

        // Probe rays.
        VkDescriptorSet rays_set = ctx.allocate_set(rays_set_layout_);
        {
          VkDescriptorImageInfo images[3]{};
          images[0] = {.imageView = rays_.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
          images[1] = {.sampler = sky_sampler_, .imageView = sky_view_,
                       .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          images[2] = {.sampler = sampler_, .imageView = irradiance_array_view_,
                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
          VkAccelerationStructureKHR tlas = raytracing.tlas(tlas_slot);
          VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
          tlas_info.accelerationStructureCount = 1;
          tlas_info.pAccelerationStructures = &tlas;
          VkDescriptorBufferInfo volume_info{volume_buffer, 0, sizeof(VolumeData)};

          VkWriteDescriptorSet writes[5];
          for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = rays_set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
          }
          writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[3].pNext = &tlas_info;
          writes[3].dstSet = rays_set;
          writes[3].dstBinding = 3;
          writes[3].descriptorCount = 1;
          writes[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
          writes[4] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[4].dstSet = rays_set;
          writes[4].dstBinding = 4;
          writes[4].descriptorCount = 1;
          writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          writes[4].pBufferInfo = &volume_info;
          vkUpdateDescriptorSets(ctx.device->device(), 5, writes, 0, nullptr);
        }
        VkDescriptorSet rays_sets[2] = {rays_set, bindless_->set()};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rays_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rays_layout_, 0, 2,
                                rays_sets, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, rays_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(rays_push), &rays_push);
        vkCmdDispatch(ctx.cmd, (kRaysPerProbe + 31) / 32, kProbeCount, 1);

        MemoryBarrier2(ctx.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        // Blend rays into both atlases.
        auto blend = [&](VkImageView atlas_view, u32 mode, u32 width, u32 height) {
          VkDescriptorSet set = ctx.allocate_set(blend_set_layout_);
          VkDescriptorImageInfo images[2]{};
          images[0] = {.imageView = atlas_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
          images[1] = {.sampler = sampler_, .imageView = rays_.view,
                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
          VkDescriptorBufferInfo volume_info{volume_buffer, 0, sizeof(VolumeData)};
          VkWriteDescriptorSet writes[3];
          for (u32 i = 0; i < 2; ++i) {
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
          }
          writes[2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[2].dstSet = set;
          writes[2].dstBinding = 2;
          writes[2].descriptorCount = 1;
          writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          writes[2].pBufferInfo = &volume_info;
          vkUpdateDescriptorSets(ctx.device->device(), 3, writes, 0, nullptr);

          BlendPush push{};
          std::memcpy(push.rotation, rays_push.rotation, sizeof(push.rotation));
          push.mode = mode;
          push.ray_count = kRaysPerProbe;
          push.reset = reset ? 1u : 0u;
          vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blend_pipeline_);
          vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blend_layout_, 0, 1,
                                  &set, 0, nullptr);
          vkCmdPushConstants(ctx.cmd, blend_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(push), &push);
          vkCmdDispatch(ctx.cmd, (width + 7) / 8, (height + 7) / 8, 1);
        };
        blend(irradiance_array_view_, 0, kIrradianceWidth, kIrradianceHeight);
        blend(distance_array_view_, 1, kDistanceWidth, kDistanceHeight);

        MemoryBarrier2(ctx.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        // Octahedral borders for bilinear wrap.
        auto border = [&](VkImageView atlas_view, u32 texels, u32 width, u32 height) {
          VkDescriptorSet set = ctx.allocate_set(border_set_layout_);
          VkDescriptorImageInfo image{.imageView = atlas_view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
          VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          write.dstSet = set;
          write.dstBinding = 0;
          write.descriptorCount = 1;
          write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          write.pImageInfo = &image;
          vkUpdateDescriptorSets(ctx.device->device(), 1, &write, 0, nullptr);

          BorderPush push{texels, kProbesX * kProbesZ, kProbesY, 0};
          vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, border_pipeline_);
          vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, border_layout_, 0, 1,
                                  &set, 0, nullptr);
          vkCmdPushConstants(ctx.cmd, border_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(push), &push);
          vkCmdDispatch(ctx.cmd, (width + 7) / 8, (height + 7) / 8, 1);
        };
        border(irradiance_array_view_, kIrradianceTexels, kIrradianceWidth, kIrradianceHeight);
        border(distance_array_view_, kDistanceTexels, kDistanceWidth, kDistanceHeight);

        MemoryBarrier2(ctx.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      });
}

DdgiSystem::~DdgiSystem() {
  VkDevice device = device_.device();
  if (rays_pipeline_) vkDestroyPipeline(device, rays_pipeline_, nullptr);
  if (rays_layout_) vkDestroyPipelineLayout(device, rays_layout_, nullptr);
  if (rays_set_layout_) vkDestroyDescriptorSetLayout(device, rays_set_layout_, nullptr);
  if (blend_pipeline_) vkDestroyPipeline(device, blend_pipeline_, nullptr);
  if (blend_layout_) vkDestroyPipelineLayout(device, blend_layout_, nullptr);
  if (blend_set_layout_) vkDestroyDescriptorSetLayout(device, blend_set_layout_, nullptr);
  if (border_pipeline_) vkDestroyPipeline(device, border_pipeline_, nullptr);
  if (border_layout_) vkDestroyPipelineLayout(device, border_layout_, nullptr);
  if (border_set_layout_) vkDestroyDescriptorSetLayout(device, border_set_layout_, nullptr);
  if (irradiance_array_view_) vkDestroyImageView(device, irradiance_array_view_, nullptr);
  if (distance_array_view_) vkDestroyImageView(device, distance_array_view_, nullptr);
  device_.DestroyImage(irradiance_);
  device_.DestroyImage(distance_);
  device_.DestroyImage(rays_);
  for (GpuBuffer& buffer : volume_buffers_) device_.DestroyBuffer(buffer);
  if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
  if (sampler_) vkDestroySampler(device, sampler_, nullptr);
}

}  // namespace rec::render

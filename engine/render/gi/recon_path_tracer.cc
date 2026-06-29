#include "render/gi/recon_path_tracer.h"

#include <array>

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/recon_atrous_cs_hlsl.h"
#include "shaders/recon_composite_cs_hlsl.h"
#include "shaders/recon_gbuffer_cs_hlsl.h"
#include "shaders/recon_temporal_cs_hlsl.h"

namespace rec::render {
namespace {

constexpr VkFormat kIrradiance = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kNormalRough = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMoments = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kViewZ = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kMotion = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kMatId = VK_FORMAT_R32_UINT;

struct GbufferPush {
  Mat4 inv_view_proj;
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 spp;
  f32 pixel_spread;
  u32 frame_index;
  u32 bounces;
};
struct TemporalPush {
  u32 size[2];
  f32 inv_size[2];
  f32 current_weight_min;
  f32 max_history;
  f32 reset;
  f32 pad;
};
struct AtrousPush {
  u32 size[2];
  u32 step_size;
  f32 normal_phi;
  f32 depth_phi;
  f32 luma_phi;
  u32 spec_mode;
  f32 spec_lobe;
};
struct CompositePush {
  u32 size[2];
  u32 debug_mode;
  f32 max_history;
};

VkDescriptorSetLayoutBinding Bind(u32 binding, VkDescriptorType type) {
  return {.binding = binding, .descriptorType = type, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}
constexpr VkDescriptorType kStorage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType kAccel = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
constexpr VkDescriptorType kCombined = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

VkDescriptorSetLayout MakeSetLayout(VkDevice device, const VkDescriptorSetLayoutBinding* b, u32 n) {
  VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = n;
  info.pBindings = b;
  VkDescriptorSetLayout l = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(device, &info, nullptr, &l);
  return l;
}
VkPipelineLayout MakePipeLayout(VkDevice device, const VkDescriptorSetLayout* sets, u32 n,
                                u32 push) {
  VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
  VkPipelineLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = n;
  info.pSetLayouts = sets;
  info.pushConstantRangeCount = 1;
  info.pPushConstantRanges = &pc;
  VkPipelineLayout l = VK_NULL_HANDLE;
  vkCreatePipelineLayout(device, &info, nullptr, &l);
  return l;
}
bool MakePipeline(VkDevice device, VkPipelineLayout layout, const unsigned char* code, size_t size,
                  VkPipeline* out) {
  VkShaderModule m = CreateShaderModule(device, code, size);
  if (m == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = m;
  info.stage.pName = "main";
  info.layout = layout;
  VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, out);
  vkDestroyShaderModule(device, m, nullptr);
  return r == VK_SUCCESS;
}

VkDescriptorImageInfo Storage(VkImageView v) {
  return {.imageView = v, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
}
VkDescriptorImageInfo Read(VkImageView v) {
  return {.imageView = v, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

}  // namespace

bool ReconPathTracer::Initialize(Device& device, VkDescriptorSetLayout bindless_layout) {
  if (bindless_layout == VK_NULL_HANDLE) return false;
  device_ = &device;
  return CreatePipelines(device, bindless_layout);
}

bool ReconPathTracer::CreatePipelines(Device& device, VkDescriptorSetLayout bindless_layout) {
  VkDevice dev = device.device();

  // gbuffer: 7 storage outputs, tlas (7), sky (8), noisy specular out (9), point
  // lights (10); set 1 bindless.
  VkDescriptorSetLayoutBinding gb[11] = {
      Bind(0, kStorage), Bind(1, kStorage), Bind(2, kStorage), Bind(3, kStorage),
      Bind(4, kStorage), Bind(5, kStorage), Bind(6, kStorage), Bind(7, kAccel),
      Bind(8, kCombined), Bind(9, kStorage), Bind(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
  gbuffer_set_ = MakeSetLayout(dev, gb, 11);
  VkDescriptorSetLayout gb_sets[2] = {gbuffer_set_, bindless_layout};
  gbuffer_layout_ = MakePipeLayout(dev, gb_sets, 2, sizeof(GbufferPush));

  VkDescriptorSetLayoutBinding tb[12] = {
      Bind(0, kStorage), Bind(1, kStorage), Bind(2, kSampled),  Bind(3, kSampled),
      Bind(4, kSampled), Bind(5, kSampled), Bind(6, kSampled),  Bind(7, kSampled),
      Bind(8, kSampled), Bind(9, kSampled), Bind(10, kSampled), Bind(11, kSampled)};
  temporal_set_ = MakeSetLayout(dev, tb, 12);
  temporal_layout_ = MakePipeLayout(dev, &temporal_set_, 1, sizeof(TemporalPush));

  VkDescriptorSetLayoutBinding ab[5] = {Bind(0, kStorage), Bind(1, kSampled), Bind(2, kSampled),
                                        Bind(3, kSampled), Bind(4, kSampled)};
  atrous_set_ = MakeSetLayout(dev, ab, 5);
  atrous_layout_ = MakePipeLayout(dev, &atrous_set_, 1, sizeof(AtrousPush));

  VkDescriptorSetLayoutBinding cb[8] = {Bind(0, kStorage), Bind(1, kSampled), Bind(2, kSampled),
                                        Bind(3, kSampled), Bind(4, kSampled), Bind(5, kSampled),
                                        Bind(6, kSampled), Bind(7, kSampled)};
  composite_set_ = MakeSetLayout(dev, cb, 8);
  composite_layout_ = MakePipeLayout(dev, &composite_set_, 1, sizeof(CompositePush));

  if (!gbuffer_set_ || !temporal_set_ || !atrous_set_ || !composite_set_) return false;

  if (!MakePipeline(dev, gbuffer_layout_, k_recon_gbuffer_cs_hlsl, sizeof(k_recon_gbuffer_cs_hlsl),
                    &gbuffer_pipeline_) ||
      !MakePipeline(dev, temporal_layout_, k_recon_temporal_cs_hlsl,
                    sizeof(k_recon_temporal_cs_hlsl), &temporal_pipeline_) ||
      !MakePipeline(dev, atrous_layout_, k_recon_atrous_cs_hlsl, sizeof(k_recon_atrous_cs_hlsl),
                    &atrous_pipeline_) ||
      !MakePipeline(dev, composite_layout_, k_recon_composite_cs_hlsl,
                    sizeof(k_recon_composite_cs_hlsl), &composite_pipeline_)) {
    REC_ERROR("recon path tracer pipeline creation failed");
    return false;
  }
  return true;
}

void ReconPathTracer::CreateBuffers(Device& device, VkExtent2D extent) {
  extent_ = extent;
  auto make = [&](PingPong& pp, VkFormat fmt) {
    for (u32 i = 0; i < 2; ++i) {
      pp.image[i] = device.CreateImage2D(fmt, extent,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
      pp.layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  };
  make(accum_, kIrradiance);
  make(moments_, kMoments);
  make(spec_accum_, kIrradiance);
  make(spec_moments_, kMoments);
  make(normal_rough_, kNormalRough);
  make(viewz_, kViewZ);
  make(matid_, kMatId);

  // Prime every owned image to GENERAL so the first frame's barriers have a
  // defined source layout (and reads of the not-yet-written prev slot are legal).
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
    for (PingPong* pp :
         {&accum_, &moments_, &spec_accum_, &spec_moments_, &normal_rough_, &viewz_, &matid_})
      for (u32 i = 0; i < 2; ++i) {
        add(pp->image[i].image);
        pp->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
      }
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<u32>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
  });
}

void ReconPathTracer::DestroyBuffers(Device& device) {
  for (PingPong* pp :
       {&accum_, &moments_, &spec_accum_, &spec_moments_, &normal_rough_, &viewz_, &matid_})
    for (u32 i = 0; i < 2; ++i)
      if (pp->image[i].image) device.DestroyImage(pp->image[i]);
}

void ReconPathTracer::Resize(Device& device, VkExtent2D extent) {
  if (extent.width == extent_.width && extent.height == extent_.height && accum_.image[0].image)
    return;
  DestroyBuffers(device);
  CreateBuffers(device, extent);
}

void ReconPathTracer::Destroy(Device& device) {
  VkDevice dev = device.device();
  DestroyBuffers(device);
  for (VkPipeline p : {gbuffer_pipeline_, temporal_pipeline_, atrous_pipeline_, composite_pipeline_})
    if (p) vkDestroyPipeline(dev, p, nullptr);
  for (VkPipelineLayout l :
       {gbuffer_layout_, temporal_layout_, atrous_layout_, composite_layout_})
    if (l) vkDestroyPipelineLayout(dev, l, nullptr);
  for (VkDescriptorSetLayout s : {gbuffer_set_, temporal_set_, atrous_set_, composite_set_})
    if (s) vkDestroyDescriptorSetLayout(dev, s, nullptr);
  gbuffer_pipeline_ = temporal_pipeline_ = atrous_pipeline_ = composite_pipeline_ = VK_NULL_HANDLE;
  // Null every handle, not just the pipelines, so a second Destroy() (double
  // shutdown / device-lost reinit) does not re-free already-freed layouts.
  gbuffer_layout_ = temporal_layout_ = atrous_layout_ = composite_layout_ = VK_NULL_HANDLE;
  gbuffer_set_ = temporal_set_ = atrous_set_ = composite_set_ = VK_NULL_HANDLE;
}

void ReconPathTracer::RunTemporal(RenderGraph& graph, ResourceHandle noisy, ResourceHandle ac_c,
                                  ResourceHandle ac_p, ResourceHandle mo_c, ResourceHandle mo_p,
                                  ResourceHandle nr_c, ResourceHandle nr_p, ResourceHandle vz_c,
                                  ResourceHandle vz_p, ResourceHandle id_c, ResourceHandle id_p,
                                  ResourceHandle motion, const Frame& frame) {
  graph.AddPass(
      "recon_temporal",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(ac_c, ResourceUsage::kStorageWrite);
        b.Write(mo_c, ResourceUsage::kStorageWrite);
        for (ResourceHandle h : {noisy, ac_p, nr_c, nr_p, vz_c, vz_p, motion, id_c, id_p, mo_p})
          b.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, ac_c, mo_c, noisy, ac_p, nr_c, nr_p, vz_c, vz_p, motion, id_c, id_p, mo_p,
       frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(temporal_set_);
        VkDescriptorImageInfo o0 = Storage(ctx.graph->image(ac_c).view);
        VkDescriptorImageInfo o1 = Storage(ctx.graph->image(mo_c).view);
        ResourceHandle reads[10] = {noisy, ac_p, nr_c, nr_p, vz_c, vz_p, motion, id_c, id_p, mo_p};
        VkDescriptorImageInfo ri[10];
        VkWriteDescriptorSet w[12];
        w[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                .descriptorCount = 1, .descriptorType = kStorage, .pImageInfo = &o0};
        w[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
                .descriptorCount = 1, .descriptorType = kStorage, .pImageInfo = &o1};
        for (u32 i = 0; i < 10; ++i) {
          ri[i] = Read(ctx.graph->image(reads[i]).view);
          w[i + 2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                      .dstBinding = i + 2, .descriptorCount = 1, .descriptorType = kSampled,
                      .pImageInfo = &ri[i]};
        }
        vkUpdateDescriptorSets(ctx.device->device(), 12, w, 0, nullptr);

        TemporalPush p{};
        p.size[0] = extent_.width; p.size[1] = extent_.height;
        p.inv_size[0] = 1.0f / extent_.width; p.inv_size[1] = 1.0f / extent_.height;
        p.current_weight_min = frame.current_weight_min;
        p.max_history = static_cast<f32>(frame.max_history);
        p.reset = frame.reset ? 1.0f : 0.0f;
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_layout_, 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, temporal_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
}

ResourceHandle ReconPathTracer::RunAtrous(RenderGraph& graph, ResourceHandle in, ResourceHandle ping,
                                          ResourceHandle pong, ResourceHandle nr_c,
                                          ResourceHandle vz_c, ResourceHandle mo_c, u32 passes,
                                          bool spec) {
  ResourceHandle denoised = in;
  for (u32 i = 0; i < passes; ++i) {
    ResourceHandle out = (i & 1u) ? pong : ping;
    graph.AddPass(
        "recon_atrous",
        [&](RenderGraph::PassBuilder& b) {
          b.Write(out, ResourceUsage::kStorageWrite);
          b.Read(in, ResourceUsage::kSampledCompute);
          b.Read(nr_c, ResourceUsage::kSampledCompute);
          b.Read(vz_c, ResourceUsage::kSampledCompute);
          b.Read(mo_c, ResourceUsage::kSampledCompute);
        },
        [this, in, out, nr_c, vz_c, mo_c, i, spec](PassContext& ctx) {
          VkDescriptorSet set = ctx.allocate_set(atrous_set_);
          VkDescriptorImageInfo o = Storage(ctx.graph->image(out).view);
          VkDescriptorImageInfo ci = Read(ctx.graph->image(in).view);
          VkDescriptorImageInfo ni = Read(ctx.graph->image(nr_c).view);
          VkDescriptorImageInfo zi = Read(ctx.graph->image(vz_c).view);
          VkDescriptorImageInfo mi = Read(ctx.graph->image(mo_c).view);
          VkDescriptorImageInfo infos[5] = {o, ci, ni, zi, mi};
          VkDescriptorType types[5] = {kStorage, kSampled, kSampled, kSampled, kSampled};
          VkWriteDescriptorSet w[5];
          for (u32 k = 0; k < 5; ++k)
            w[k] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = k,
                    .descriptorCount = 1, .descriptorType = types[k], .pImageInfo = &infos[k]};
          vkUpdateDescriptorSets(ctx.device->device(), 5, w, 0, nullptr);

          AtrousPush p{};
          p.size[0] = extent_.width; p.size[1] = extent_.height;
          p.step_size = 1u << i;
          p.normal_phi = 64.0f;
          p.depth_phi = 80.0f;
          p.luma_phi = 4.0f;
          p.spec_mode = spec ? 1u : 0u;
          p.spec_lobe = 8.0f;  // smooth reflectors keep tight lobes, rough ones filter normally
          vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, atrous_pipeline_);
          vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, atrous_layout_, 0, 1,
                                  &set, 0, nullptr);
          vkCmdPushConstants(ctx.cmd, atrous_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
          vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
        });
    in = out;
    denoised = out;
  }
  return denoised;
}

void ReconPathTracer::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                                 VkDescriptorSet bindless_set, VkImageView sky_view,
                                 VkSampler sky_sampler, ResourceHandle output, const Frame& frame) {
  u32 cur = frame.frame_index & 1u;
  u32 prv = 1u - cur;
  auto imp = [&](const char* name, PingPong& pp, u32 i) {
    return graph.ImportImage(name, pp.image[i], &pp.layout[i]);
  };

  // Shared gbuffer history (both signals reproject/reject against the same surface).
  ResourceHandle nr_c = imp("recon_nr_c", normal_rough_, cur);
  ResourceHandle nr_p = imp("recon_nr_p", normal_rough_, prv);
  ResourceHandle vz_c = imp("recon_vz_c", viewz_, cur);
  ResourceHandle vz_p = imp("recon_vz_p", viewz_, prv);
  ResourceHandle id_c = imp("recon_id_c", matid_, cur);
  ResourceHandle id_p = imp("recon_id_p", matid_, prv);
  // Diffuse + specular history.
  ResourceHandle ac_c = imp("recon_ac_c", accum_, cur);
  ResourceHandle ac_p = imp("recon_ac_p", accum_, prv);
  ResourceHandle mo_c = imp("recon_mo_c", moments_, cur);
  ResourceHandle mo_p = imp("recon_mo_p", moments_, prv);
  ResourceHandle sac_c = imp("recon_sac_c", spec_accum_, cur);
  ResourceHandle sac_p = imp("recon_sac_p", spec_accum_, prv);
  ResourceHandle smo_c = imp("recon_smo_c", spec_moments_, cur);
  ResourceHandle smo_p = imp("recon_smo_p", spec_moments_, prv);

  auto tex = [&](const char* name, VkFormat fmt) {
    return graph.CreateTexture({.name = name, .format = fmt, .width = extent_.width,
                                .height = extent_.height});
  };
  ResourceHandle noisy = tex("recon_noisy", kIrradiance);
  ResourceHandle spec_noisy = tex("recon_spec_noisy", kIrradiance);
  ResourceHandle motion = tex("recon_motion", kMotion);
  ResourceHandle albedo = tex("recon_albedo", kIrradiance);
  ResourceHandle emissive = tex("recon_emissive", kIrradiance);
  ResourceHandle ping = tex("recon_ping", kIrradiance);
  ResourceHandle pong = tex("recon_pong", kIrradiance);
  ResourceHandle spec_ping = tex("recon_spec_ping", kIrradiance);
  ResourceHandle spec_pong = tex("recon_spec_pong", kIrradiance);

  // --- 1. gbuffer ---
  graph.AddPass(
      "recon_gbuffer",
      [&](RenderGraph::PassBuilder& b) {
        for (ResourceHandle h : {noisy, nr_c, vz_c, motion, id_c, albedo, emissive, spec_noisy})
          b.Write(h, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, bindless_set, sky_view, sky_sampler, noisy, nr_c, vz_c, motion,
       id_c, albedo, emissive, spec_noisy, frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(gbuffer_set_);
        ResourceHandle outs[7] = {noisy, nr_c, vz_c, motion, id_c, albedo, emissive};
        VkDescriptorImageInfo si[8];
        VkWriteDescriptorSet w[11];
        for (u32 i = 0; i < 7; ++i) {
          si[i] = Storage(ctx.graph->image(outs[i]).view);
          w[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
                  .descriptorCount = 1, .descriptorType = kStorage, .pImageInfo = &si[i]};
        }
        VkAccelerationStructureKHR tlas = raytracing.tlas(tlas_slot);
        VkWriteDescriptorSetAccelerationStructureKHR ai{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        ai.accelerationStructureCount = 1;
        ai.pAccelerationStructures = &tlas;
        w[7] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &ai, .dstSet = set,
                .dstBinding = 7, .descriptorCount = 1, .descriptorType = kAccel};
        VkDescriptorImageInfo sky{.sampler = sky_sampler, .imageView = sky_view,
                                  .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[8] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 8,
                .descriptorCount = 1, .descriptorType = kCombined, .pImageInfo = &sky};
        si[7] = Storage(ctx.graph->image(spec_noisy).view);
        w[9] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 9,
                .descriptorCount = 1, .descriptorType = kStorage, .pImageInfo = &si[7]};
        VkDescriptorBufferInfo lbi{frame.lights, 0,
                                   frame.lights_size ? frame.lights_size : VK_WHOLE_SIZE};
        w[10] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 10,
                 .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .pBufferInfo = &lbi};
        vkUpdateDescriptorSets(ctx.device->device(), 11, w, 0, nullptr);

        GbufferPush p{};
        p.inv_view_proj = frame.inv_view_proj;
        p.view_proj = frame.view_proj;
        p.prev_view_proj = frame.prev_view_proj;
        p.camera_pos[0] = frame.camera_pos.x; p.camera_pos[1] = frame.camera_pos.y;
        p.camera_pos[2] = frame.camera_pos.z;
        p.camera_pos[3] = static_cast<f32>(frame.light_count);  // point-light count
        Vec3 sun = Normalize(frame.sun_direction);
        p.sun_direction[0] = sun.x; p.sun_direction[1] = sun.y; p.sun_direction[2] = sun.z;
        p.sun_direction[3] = frame.sun_intensity;
        p.sun_color[0] = frame.sun_color.x; p.sun_color[1] = frame.sun_color.y;
        p.sun_color[2] = frame.sun_color.z; p.sun_color[3] = frame.sun_radius;
        p.spp = frame.spp < 1 ? 1u : frame.spp;
        p.pixel_spread = frame.pixel_spread;
        p.frame_index = frame.frame_index;
        p.bounces = bounces_;
        VkDescriptorSet sets[2] = {set, bindless_set};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gbuffer_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gbuffer_layout_, 0, 2, sets,
                                0, nullptr);
        vkCmdPushConstants(ctx.cmd, gbuffer_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });

  // --- 2. temporal accumulation (diffuse + specular share the gbuffer history) ---
  RunTemporal(graph, noisy, ac_c, ac_p, mo_c, mo_p, nr_c, nr_p, vz_c, vz_p, id_c, id_p, motion,
              frame);
  RunTemporal(graph, spec_noisy, sac_c, sac_p, smo_c, smo_p, nr_c, nr_p, vz_c, vz_p, id_c, id_p,
              motion, frame);

  // --- 3. a-trous (N passes, ping-pong) for each signal ---
  u32 passes = frame.atrous_passes == 0 ? 1u : frame.atrous_passes;
  ResourceHandle denoised = RunAtrous(graph, ac_c, ping, pong, nr_c, vz_c, mo_c, passes, false);
  ResourceHandle spec_denoised =
      RunAtrous(graph, sac_c, spec_ping, spec_pong, nr_c, vz_c, smo_c, passes, true);

  // --- 4. composite ---
  graph.AddPass(
      "recon_composite",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(output, ResourceUsage::kStorageWrite);
        for (ResourceHandle h : {albedo, denoised, emissive, mo_c, nr_c, motion, spec_denoised})
          b.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, output, albedo, denoised, emissive, mo_c, nr_c, motion, spec_denoised,
       frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(composite_set_);
        VkDescriptorImageInfo o = Storage(ctx.graph->image(output).view);
        ResourceHandle reads[7] = {albedo, denoised, emissive, mo_c, nr_c, motion, spec_denoised};
        VkDescriptorImageInfo ri[7];
        VkWriteDescriptorSet w[8];
        w[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                .descriptorCount = 1, .descriptorType = kStorage, .pImageInfo = &o};
        for (u32 i = 0; i < 7; ++i) {
          ri[i] = Read(ctx.graph->image(reads[i]).view);
          w[i + 1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                      .dstBinding = i + 1, .descriptorCount = 1, .descriptorType = kSampled,
                      .pImageInfo = &ri[i]};
        }
        vkUpdateDescriptorSets(ctx.device->device(), 8, w, 0, nullptr);

        CompositePush p{};
        p.size[0] = extent_.width; p.size[1] = extent_.height;
        p.debug_mode = frame.debug_mode;
        p.max_history = static_cast<f32>(frame.max_history);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_layout_, 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, composite_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p),
                           &p);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
}

}  // namespace rec::render

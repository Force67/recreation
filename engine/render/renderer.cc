#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <stb_image_write.h>

#include "core/log.h"

namespace rec::render {

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Initialize(const RendererDesc& desc, Window& window) {
  desc_ = desc;
  settings_.aa_mode = desc.aa_mode;
  settings_.upscaler = desc.upscaler;
  settings_.rt_shadows = desc.raytracing.shadows;
  output_width_ = window.width();
  output_height_ = window.height();

  window_ = &window;
  device_ = Device::Create({.enable_validation = desc.enable_validation,
                            .request_raytracing = desc.enable_raytracing},
                           window);
  if (device_->is_stub()) {
    REC_WARN("renderer running in stub mode");
    return true;
  }

  swapchain_ = Swapchain::Create(*device_, output_width_, output_height_, settings_.vsync);
  if (!swapchain_ || !CreateFrameResources() || !CreateRenderFinishedSemaphores()) return false;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  if (desc.enable_raytracing && device_->caps().raytracing) {
    raytracing_ = RayTracingContext::Create(*device_);
    raytracing_->Configure(desc.raytracing);
  }
  rt_available_ = raytracing_ && device_->caps().ray_query;

  transient_pool_ = std::make_unique<TransientPool>(*device_);
  if (rt_available_) {
    bindless_ = BindlessRegistry::Create(*device_);
    if (!bindless_) return false;
  }
  material_system_ = MaterialSystem::Create(*device_, bindless_.get());
  if (!material_system_) return false;
  environment_ = EnvironmentSystem::Create(*device_);
  if (!environment_) return false;
  mesh_pipeline_ = MeshPipeline::Create(*device_, kSceneColorFormat, kMotionFormat,
                                        kNormalFormat, kDepthFormat,
                                        material_system_->set_layout(),
                                        environment_->env_set_layout());
  post_ = PostPass::Create(*device_, swapchain_->format());
  if (!mesh_pipeline_ || !post_ || !taa_.Initialize(*device_)) return false;
  if (rt_available_ && !rtao_.Initialize(*device_)) return false;
  if (!bloom_.Initialize(*device_) || !exposure_.Initialize(*device_)) return false;
  if (rt_available_) {
    ddgi_ = DdgiSystem::Create(*device_, environment_->sky_view(), environment_->sampler(),
                               *bindless_);
    if (!ddgi_) return false;
    water_ = WaterPass::Create(*device_, kSceneColorFormat, kMotionFormat, kDepthFormat,
                               mesh_pipeline_->set_layout(), material_system_->set_layout(),
                               environment_->env_set_layout(), bindless_->set_layout());
    if (!water_) return false;
  }
  if (!environment_->CreateSkyPipeline(mesh_pipeline_->set_layout(), kSceneColorFormat,
                                       kMotionFormat, kDepthFormat)) {
    return false;
  }

  if (settings_.upscaler != UpscalerKind::kNone && !CreateUpscalerForSettings()) {
    REC_WARN("upscaler unavailable, falling back to taa");
    settings_.upscaler = UpscalerKind::kNone;
    settings_.aa_mode = AntiAliasingMode::kTaa;
  }
  applied_upscaler_ = settings_.upscaler;
  applied_quality_ = settings_.upscaler_quality;
  applied_aa_ = settings_.aa_mode;
  applied_vsync_ = settings_.vsync;

  UpdateRenderResolution();
  taa_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});

  // Debug captures without window manager screenshots:
  // REC_SCREENSHOT=/tmp/frame.png:12 saves the frame at t=12s.
  if (const char* spec = std::getenv("REC_SCREENSHOT")) {
    std::string value = spec;
    size_t colon = value.find_last_of(':');
    if (colon != std::string::npos) {
      screenshot_at_ = std::atof(value.c_str() + colon + 1);
      value.resize(colon);
    }
    screenshot_path_ = value;
  }

  return true;
}

void Renderer::CaptureScreenshot(const std::string& path) {
  screenshot_path_ = path;
  screenshot_at_ = -1;
}

void Renderer::WriteScreenshot(u32 image_index) {
  device_->WaitIdle();
  VkExtent2D extent = swapchain_->extent();
  u64 size = static_cast<u64>(extent.width) * extent.height * 4;
  GpuBuffer staging = device_->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  if (!staging.mapped) return;

  device_->ImmediateSubmit([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = swapchain_->image(image_index);
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, swapchain_->image(image_index),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &dep);
  });

  // Swapchain is bgra; png wants rgb.
  base::Vector<u8> pixels(static_cast<size_t>(extent.width) * extent.height * 3);
  const u8* src = static_cast<const u8*>(staging.mapped);
  for (size_t i = 0; i < static_cast<size_t>(extent.width) * extent.height; ++i) {
    pixels[i * 3 + 0] = src[i * 4 + 2];
    pixels[i * 3 + 1] = src[i * 4 + 1];
    pixels[i * 3 + 2] = src[i * 4 + 0];
  }
  device_->DestroyBuffer(staging);
  if (stbi_write_png(screenshot_path_.c_str(), static_cast<int>(extent.width),
                     static_cast<int>(extent.height), 3, pixels.data(),
                     static_cast<int>(extent.width) * 3)) {
    REC_INFO("screenshot written: {}", screenshot_path_);
  } else {
    REC_WARN("screenshot write failed: {}", screenshot_path_);
  }
  screenshot_path_.clear();
}

bool Renderer::CreateUpscalerForSettings() {
  f32 scale = UpscalerScale(settings_.upscaler_quality);
  u32 render_width = static_cast<u32>(static_cast<f32>(output_width_) / scale);
  u32 render_height = static_cast<u32>(static_cast<f32>(output_height_) / scale);
  upscaler_ = CreateUpscaler({.kind = settings_.upscaler,
                              .render_width = render_width,
                              .render_height = render_height,
                              .output_width = output_width_,
                              .output_height = output_height_,
                              .sharpness = settings_.sharpness},
                             *device_);
  if (upscaler_) {
    settings_.aa_mode = AntiAliasingMode::kUpscaler;
    return true;
  }
  return false;
}

void Renderer::UpdateRenderResolution() {
  if (upscaler_ && settings_.aa_mode == AntiAliasingMode::kUpscaler) {
    f32 scale = UpscalerScale(settings_.upscaler_quality);
    render_width_ = static_cast<u32>(static_cast<f32>(output_width_) / scale);
    render_height_ = static_cast<u32>(static_cast<f32>(output_height_) / scale);
  } else {
    render_width_ = output_width_;
    render_height_ = output_height_;
  }
}

void Renderer::ApplySettings() {
  if (settings_.vsync != applied_vsync_) {
    applied_vsync_ = settings_.vsync;
    RecreateSwapchain();
  }

  // kUpscaler is only valid with a live upscaler.
  if (settings_.aa_mode == AntiAliasingMode::kUpscaler && settings_.upscaler == UpscalerKind::kNone) {
    settings_.aa_mode = AntiAliasingMode::kTaa;
  }

  bool upscaler_changed = settings_.upscaler != applied_upscaler_ ||
                          settings_.upscaler_quality != applied_quality_;
  if (upscaler_changed) {
    device_->WaitIdle();
    upscaler_.reset();
    if (settings_.upscaler != UpscalerKind::kNone) {
      if (!CreateUpscalerForSettings()) {
        REC_WARN("upscaler unavailable, falling back to taa");
        settings_.upscaler = UpscalerKind::kNone;
        settings_.aa_mode = AntiAliasingMode::kTaa;
      }
    } else if (settings_.aa_mode == AntiAliasingMode::kUpscaler) {
      settings_.aa_mode = AntiAliasingMode::kTaa;
    }
    applied_upscaler_ = settings_.upscaler;
    applied_quality_ = settings_.upscaler_quality;
    UpdateRenderResolution();
    transient_pool_->Clear();
    taa_.Resize(*device_, {render_width_, render_height_});
    if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
    taa_.Reset();
    has_prev_frame_ = false;
  }

  if (settings_.aa_mode != applied_aa_) {
    bool resolution_changes = settings_.aa_mode == AntiAliasingMode::kUpscaler ||
                              applied_aa_ == AntiAliasingMode::kUpscaler;
    applied_aa_ = settings_.aa_mode;
    if (resolution_changes) {
      device_->WaitIdle();
      UpdateRenderResolution();
      transient_pool_->Clear();
      taa_.Resize(*device_, {render_width_, render_height_});
      if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
    }
    taa_.Reset();
    has_prev_frame_ = false;
  }

  taa_.Configure({.history_blend = settings_.taa_history_blend,
                  .jitter_sample_count = taa_.settings().jitter_sample_count});
  rtao_.Configure({.radius = settings_.ao_radius,
                   .intensity = settings_.ao_intensity,
                   .ray_count = settings_.ao_rays == 0 ? 1 : settings_.ao_rays});
  exposure_.Configure({.automatic = settings_.auto_exposure,
                       .compensation = settings_.exposure,
                       .adaptation_speed = settings_.adaptation_speed,
                       .manual_exposure = settings_.exposure});
  if (ddgi_) {
    ddgi_->Configure({.probe_spacing = settings_.ddgi_spacing,
                      .hysteresis = 0.97f,
                      .energy_scale = settings_.ddgi_intensity});
  }

  Vec3 sun = Normalize(settings_.sun_direction);
  bool sun_changed = sun.x != applied_sun_direction_.x || sun.y != applied_sun_direction_.y ||
                     sun.z != applied_sun_direction_.z ||
                     settings_.sun_intensity != applied_sun_intensity_ ||
                     settings_.sun_color.x != applied_sun_color_.x ||
                     settings_.sun_color.y != applied_sun_color_.y ||
                     settings_.sun_color.z != applied_sun_color_.z;
  if (sun_changed) {
    applied_sun_direction_ = sun;
    applied_sun_intensity_ = settings_.sun_intensity;
    applied_sun_color_ = settings_.sun_color;
    environment_dirty_ = true;
  }
}

bool Renderer::UploadMesh(const asset::Mesh& mesh) {
  if (!device_ || device_->is_stub()) return false;
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) return false;

  const asset::MeshLod& lod = mesh.lods[0];
  VkBufferUsageFlags rt_usage =
      raytracing_ ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                  : 0;
  GpuMesh gpu;
  gpu.vertices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.vertices.data()),
               lod.vertices.size() * sizeof(asset::Vertex)),
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rt_usage);
  gpu.indices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.indices.data()), lod.indices.size() * sizeof(u32)),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rt_usage);
  gpu.index_count = static_cast<u32>(lod.indices.size());
  gpu.vertex_count = static_cast<u32>(lod.vertices.size());
  // Skinned meshes carry a parallel bone index/weight stream, bound as a second
  // vertex buffer by the skinned pipeline.
  if (mesh.skinned && lod.skinning.size() == lod.vertices.size()) {
    gpu.skinning = device_->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8*>(lod.skinning.data()),
                 lod.skinning.size() * sizeof(asset::SkinnedVertexExtra)),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpu.skinned = gpu.skinning.buffer != VK_NULL_HANDLE;
  }
  if (lod.submeshes.empty()) {
    gpu.submeshes.push_back({0, gpu.index_count, 0, false});
  } else {
    for (const asset::Submesh& submesh : lod.submeshes) {
      bool water = material_system_ && material_system_->is_water(submesh.material.hash);
      bool blend = water || (material_system_ && material_system_->is_blend(submesh.material.hash));
      gpu.submeshes.push_back(
          {submesh.index_offset, submesh.index_count, submesh.material.hash, blend, water});
    }
  }
  gpu.all_blend = true;
  for (const GpuSubmesh& submesh : gpu.submeshes) {
    if (!submesh.blend) gpu.all_blend = false;
  }
  gpu.no_rt = mesh.exclude_from_rt;
  if (bindless_ && !gpu.all_blend && !gpu.no_rt) {
    base::Vector<BindlessRegistry::GeometryRecord> geometries;
    for (const GpuSubmesh& submesh : gpu.submeshes) {
      if (submesh.blend || submesh.index_count == 0) continue;
      geometries.push_back({submesh.index_offset,
                            material_system_->bindless_material(submesh.material)});
    }
    gpu.bindless_index = bindless_->RegisterMesh(gpu.vertices.buffer, gpu.indices.buffer,
                                                 geometries.data(),
                                                 static_cast<u32>(geometries.size()));
    if (gpu.bindless_index == BindlessRegistry::kInvalidIndex) gpu.bindless_index = 0;
  }
  meshes_[mesh.id.hash] = gpu;
  // Pure transparency never enters the tlas: water occluding rtao and
  // shadow rays would black out everything under it.
  if (raytracing_ && !gpu.all_blend && !gpu.no_rt) raytracing_->BuildBlas(mesh.id.hash, gpu);
  return true;
}

bool Renderer::UploadTexture(const asset::Texture& texture) {
  if (!material_system_) return false;
  return material_system_->UploadTexture(texture);
}

bool Renderer::UploadMaterial(const asset::Material& material) {
  if (!material_system_) return false;
  return material_system_->UploadMaterial(material);
}

void Renderer::RenderFrame(const FrameView& view) {
  if (!device_ || device_->is_stub() || !swapchain_) return;

  ApplySettings();

  FrameResources& frame = frames_[frame_index_ % kFramesInFlight];
  vkWaitForFences(device_->device(), 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

  u32 image_index = 0;
  VkResult acquired = swapchain_->Acquire(frame.image_available, &image_index);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    RecreateSwapchain();
    return;
  }
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return;

  vkResetCommandPool(device_->device(), frame.pool, 0);
  vkResetDescriptorPool(device_->device(), frame.descriptor_pool, 0);

  transient_pool_->BeginFrame();
  graph_.Reset();
  BuildFrameGraph(frame, image_index, view);
  if (!graph_.Compile(*device_, *transient_pool_)) return;

  // Only reset once the frame is guaranteed to submit, so an early return
  // above cannot deadlock the next wait.
  vkResetFences(device_->device(), 1, &frame.in_flight);

  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.cmd, &begin);

  PassContext ctx;
  ctx.cmd = frame.cmd;
  ctx.device = device_.get();
  ctx.allocate_set = [this, &frame](VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = frame.descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device_->device(), &info, &set);
    return set;
  };
  graph_.Execute(ctx);

  vkEndCommandBuffer(frame.cmd);

  VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = frame.image_available;
  wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = render_finished_[image_index];
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cmd_info.commandBuffer = frame.cmd;

  VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &wait;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd_info;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  vkQueueSubmit2(device_->graphics_queue(), 1, &submit, frame.in_flight);

  if (!screenshot_path_.empty() && time_seconds_ >= screenshot_at_) {
    WriteScreenshot(image_index);
  }

  VkResult presented = swapchain_->Present(render_finished_[image_index], image_index);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
    RecreateSwapchain();
  }
  ++frame_index_;
}

void Renderer::BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view) {
  bool rt_shadows = rt_available_ && settings_.rt_shadows;
  bool rtao_active = rt_available_ && settings_.rtao;
  bool ddgi_active = ddgi_ && settings_.ddgi && settings_.ibl;
  time_seconds_ += view.frame_delta_seconds;

  // Transparent work is gathered up front: water forces a tlas (the water
  // pipeline statically binds it) and an opaque snapshot pass.
  struct TransparentDraw {
    const DrawItem* item;
    const GpuSubmesh* submesh;
    f32 distance_sq;
  };
  base::Vector<TransparentDraw> transparent;
  bool any_water = false;
  for (const DrawItem& item : view.draws) {
    const GpuMesh* mesh = meshes_.find(item.mesh);
    if (!mesh) continue;
    for (const GpuSubmesh& submesh : mesh->submeshes) {
      if (!submesh.blend) continue;
      f32 dx = item.transform.m[12] - view.camera.eye.x;
      f32 dy = item.transform.m[13] - view.camera.eye.y;
      f32 dz = item.transform.m[14] - view.camera.eye.z;
      transparent.push_back({&item, &submesh, dx * dx + dy * dy + dz * dz});
      if (submesh.water) any_water = true;
    }
  }
  bool water_pipeline_active = any_water && water_ != nullptr;

  // Camera state for both this frame and reprojection. Jitter lives in the
  // projection, not the matrices used for motion vectors.
  f32 aspect = static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
  Mat4 proj = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f);
  Mat4 view_proj = proj * LookAt(view.camera.eye, view.camera.target, {0, 1, 0});

  bool temporal =
      settings_.aa_mode == AntiAliasingMode::kTaa ||
      settings_.aa_mode == AntiAliasingMode::kUpscaler;
  f32 jitter_x = 0, jitter_y = 0;
  if (temporal) {
    u32 sample_count = taa_.settings().jitter_sample_count;
    if (settings_.aa_mode == AntiAliasingMode::kUpscaler) {
      // FSR-style phase count grows with the scale factor squared.
      f32 scale = static_cast<f32>(output_width_) / static_cast<f32>(render_width_);
      sample_count = static_cast<u32>(std::ceil(8.0f * scale * scale));
    }
    JitterSequence::Sample(frame_index_, sample_count, &jitter_x, &jitter_y);
  }

  bool first_frame = !has_prev_frame_;

  FrameGlobals globals;
  globals.view_proj = view_proj;
  globals.prev_view_proj = has_prev_frame_ ? prev_view_proj_ : view_proj;
  globals.inv_view_proj = Inverse(view_proj);
  globals.jitter[0] = 2.0f * jitter_x / static_cast<f32>(render_width_);
  globals.jitter[1] = 2.0f * jitter_y / static_cast<f32>(render_height_);
  Vec3 sun = Normalize(settings_.sun_direction);
  globals.sun_direction[0] = sun.x;
  globals.sun_direction[1] = sun.y;
  globals.sun_direction[2] = sun.z;
  globals.sun_direction[3] = settings_.sun_intensity;
  globals.sun_color[0] = settings_.sun_color.x;
  globals.sun_color[1] = settings_.sun_color.y;
  globals.sun_color[2] = settings_.sun_color.z;
  globals.sun_color[3] = settings_.ambient;
  globals.camera_position[0] = view.camera.eye.x;
  globals.camera_position[1] = view.camera.eye.y;
  globals.camera_position[2] = view.camera.eye.z;
  globals.camera_position[3] = settings_.ibl_intensity;
  globals.misc[0] = static_cast<f32>(render_width_);
  globals.misc[1] = static_cast<f32>(render_height_);
  globals.misc[2] = settings_.sun_angular_radius;
  globals.misc[3] = static_cast<f32>(frame_index_ % 4096);
  if (settings_.ibl) globals.flags |= kFrameFlagIbl;
  if (rtao_active) globals.flags |= kFrameFlagAoValid;
  if (ddgi_active) globals.flags |= kFrameFlagDdgi;
  if (water_pipeline_active && settings_.water_reflections) globals.flags |= kFrameFlagWaterRt;
  globals.time = static_cast<f32>(time_seconds_);
  std::memcpy(frame.globals.mapped, &globals, sizeof(globals));
  prev_view_proj_ = view_proj;
  has_prev_frame_ = true;

  // Skinning palette for every skinned draw this frame, read by device address.
  if (!view.bone_matrices.empty() && frame.bone_palette.mapped) {
    u32 count = std::min<u32>(static_cast<u32>(view.bone_matrices.size()), kMaxFrameBones);
    std::memcpy(frame.bone_palette.mapped, view.bone_matrices.data(), count * sizeof(Mat4));
  }

  ResourceHandle scene_color = graph_.CreateTexture(
      {.name = "scene_color", .format = kSceneColorFormat, .width = render_width_,
       .height = render_height_});
  ResourceHandle motion = graph_.CreateTexture(
      {.name = "motion", .format = kMotionFormat, .width = render_width_,
       .height = render_height_});
  ResourceHandle depth = graph_.CreateTexture(
      {.name = "depth", .format = kDepthFormat, .width = render_width_,
       .height = render_height_});
  ResourceHandle normals = graph_.CreateTexture(
      {.name = "normals", .format = kNormalFormat, .width = render_width_,
       .height = render_height_});
  // Raw reversed z exported by the prepass; every depth consumer samples
  // this so the real depth attachment never changes layout mid frame
  // (sampling round trips corrupt its compression metadata on nvidia).
  ResourceHandle depth_export = graph_.CreateTexture(
      {.name = "depth_export", .format = VK_FORMAT_R32_SFLOAT, .width = render_width_,
       .height = render_height_});

  if (environment_dirty_ && (settings_.ibl || settings_.sky)) {
    environment_dirty_ = false;
    Vec3 env_sun = applied_sun_direction_;
    f32 env_intensity = applied_sun_intensity_;
    Vec3 env_color = applied_sun_color_;
    graph_.AddPass(
        "env_update", [](RenderGraph::PassBuilder&) {},
        [this, env_sun, env_intensity, env_color](PassContext& ctx) {
          environment_->RecordUpdate(ctx.cmd, env_sun, env_intensity, env_color);
        });
  }

  u32 tlas_slot = frame_index_ % RayTracingContext::kSlots;
  if (rt_shadows || rtao_active || ddgi_active || water_pipeline_active) {
    base::Vector<RayTracingContext::Instance> instances;
    instances.reserve(view.draws.size());
    for (const DrawItem& item : view.draws) {
      const GpuMesh* mesh = meshes_.find(item.mesh);
      if (!mesh || mesh->all_blend || mesh->no_rt) continue;
      instances.push_back({.mesh_key = item.mesh,
                           .custom_index = mesh->bindless_index,
                           .transform = item.transform});
    }
    graph_.AddPass(
        "tlas_build", [](RenderGraph::PassBuilder&) {},
        [this, tlas_slot, instances = std::move(instances)](PassContext& ctx) {
          raytracing_->BuildTlas(ctx.cmd, tlas_slot, instances);
        });
  }

  graph_.AddPass(
      "prepass",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(normals, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Write(depth_export, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, normals, motion, depth_export, depth, &frame, &view](PassContext& ctx) {
        VkRenderingAttachmentInfo colors[3];
        colors[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[0].imageView = ctx.graph->image(normals).view;
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[1] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[1].imageView = ctx.graph->image(motion).view;
        colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[2] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[2].imageView = ctx.graph->image(depth_export).view;
        colors[2].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo depth_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_attachment.imageView = ctx.graph->image(depth).view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clearValue.depthStencil = {0.0f, 0};  // reversed z clears to far = 0

        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {render_width_, render_height_}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 3;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = &depth_attachment;
        vkCmdBeginRendering(ctx.cmd, &rendering);

        VkViewport viewport{0, 0, static_cast<f32>(render_width_),
                            static_cast<f32>(render_height_), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {render_width_, render_height_}};
        vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

        VkDescriptorSet globals_set = ctx.allocate_set(mesh_pipeline_->set_layout());
        VkDescriptorBufferInfo buffer_info{frame.globals.buffer, 0, sizeof(FrameGlobals)};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = globals_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);

        mesh_pipeline_->BindPrepass(ctx.cmd, globals_set);
        VkDescriptorSet bound_material = VK_NULL_HANDLE;
        bool skinned_bound = false;
        for (const DrawItem& item : view.draws) {
          const GpuMesh* mesh = meshes_.find(item.mesh);
          if (!mesh || mesh->all_blend) continue;
          bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
          if (draw_skinned != skinned_bound) {
            mesh_pipeline_->SetPrepassSkinned(ctx.cmd, draw_skinned);
            skinned_bound = draw_skinned;
          }
          MeshPushConstants push{.model = item.transform, .prev_model = item.prev_transform};
          if (draw_skinned && item.skin_offset >= 0) {
            push.bone_address = frame.bone_palette_address;
            push.skin_offset = static_cast<u32>(item.skin_offset);
          }
          mesh_pipeline_->Draw(ctx.cmd, *mesh, push);
          for (const GpuSubmesh& submesh : mesh->submeshes) {
            if (submesh.blend) continue;  // transparency owns its own depth
            VkDescriptorSet material = material_system_->set(submesh.material);
            if (material != bound_material) {
              mesh_pipeline_->BindMaterial(ctx.cmd, material);
              bound_material = material;
            }
            mesh_pipeline_->DrawSubmesh(ctx.cmd, submesh);
          }
        }
        vkCmdEndRendering(ctx.cmd);
      });

  if (ddgi_active) {
    ddgi_->AddToGraph(graph_, *raytracing_, tlas_slot, view.camera.eye,
                      applied_sun_direction_, applied_sun_intensity_, applied_sun_color_,
                      frame_index_);
  }

  ResourceHandle ao = kInvalidResource;
  if (rtao_active) {
    ao = rtao_.AddToGraph(graph_, *raytracing_, tlas_slot, depth_export, normals, motion,
                          globals.inv_view_proj, frame_index_);
  }

  graph_.AddPass(
      "scene",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(scene_color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
        if (ao != kInvalidResource) builder.Read(ao, ResourceUsage::kSampledFragment);
      },
      [this, scene_color, motion, depth, ao, tlas_slot, rt_shadows, ddgi_active, &frame,
       &view](PassContext& ctx) {
        VkRenderingAttachmentInfo colors[2];
        colors[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[0].imageView = ctx.graph->image(scene_color).view;
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[0].clearValue.color = {{0.02f, 0.02f, 0.05f, 1.0f}};
        colors[1] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[1].imageView = ctx.graph->image(motion).view;
        colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // the prepass wrote motion
        colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo depth_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_attachment.imageView = ctx.graph->image(depth).view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // prepass depth, tested EQUAL
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {render_width_, render_height_}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 2;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = &depth_attachment;
        vkCmdBeginRendering(ctx.cmd, &rendering);

        VkViewport viewport{0, 0, static_cast<f32>(render_width_),
                            static_cast<f32>(render_height_), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {render_width_, render_height_}};
        vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

        VkDescriptorSet globals_set = ctx.allocate_set(mesh_pipeline_->set_layout());
        VkDescriptorBufferInfo buffer_info{frame.globals.buffer, 0, sizeof(FrameGlobals)};
        VkWriteDescriptorSet writes[2];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = globals_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        u32 write_count = 1;

        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        if (rt_shadows) {
          tlas = raytracing_->tlas(tlas_slot);
          tlas_info.accelerationStructureCount = 1;
          tlas_info.pAccelerationStructures = &tlas;
          writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[1].pNext = &tlas_info;
          writes[1].dstSet = globals_set;
          writes[1].dstBinding = 1;
          writes[1].descriptorCount = 1;
          writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
          write_count = 2;
        }
        vkUpdateDescriptorSets(device_->device(), write_count, writes, 0, nullptr);

        VkDescriptorSet env_set = ctx.allocate_set(environment_->env_set_layout());
        VkImageView ao_view =
            ao != kInvalidResource ? ctx.graph->image(ao).view : VK_NULL_HANDLE;
        EnvironmentSystem::DdgiBinding ddgi_binding;
        if (ddgi_active) ddgi_binding = ddgi_->binding(frame_index_);
        environment_->WriteEnvSet(env_set, ao_view, ddgi_active ? &ddgi_binding : nullptr);

        mesh_pipeline_->Bind(ctx.cmd, globals_set, env_set, rt_shadows, settings_.wireframe);
        VkDescriptorSet bound_material = VK_NULL_HANDLE;
        bool skinned_bound = false;
        for (const DrawItem& item : view.draws) {
          const GpuMesh* mesh = meshes_.find(item.mesh);
          if (!mesh || mesh->all_blend) continue;
          bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
          if (draw_skinned != skinned_bound) {
            mesh_pipeline_->SetSkinned(ctx.cmd, draw_skinned, rt_shadows, settings_.wireframe);
            skinned_bound = draw_skinned;
          }
          MeshPushConstants push{.model = item.transform, .prev_model = item.prev_transform};
          if (draw_skinned && item.skin_offset >= 0) {
            push.bone_address = frame.bone_palette_address;
            push.skin_offset = static_cast<u32>(item.skin_offset);
          }
          mesh_pipeline_->Draw(ctx.cmd, *mesh, push);
          for (const GpuSubmesh& submesh : mesh->submeshes) {
            if (submesh.blend) continue;
            VkDescriptorSet material = material_system_->set(submesh.material);
            if (material != bound_material) {
              mesh_pipeline_->BindMaterial(ctx.cmd, material);
              bound_material = material;
            }
            mesh_pipeline_->DrawSubmesh(ctx.cmd, submesh);
          }
        }
        if (skinned_bound) {
          mesh_pipeline_->SetSkinned(ctx.cmd, false, rt_shadows, settings_.wireframe);
        }
        if (settings_.sky) environment_->DrawSky(ctx.cmd, globals_set);
        vkCmdEndRendering(ctx.cmd);
      });

  ResourceHandle lit = scene_color;
  if (!transparent.empty() && water_) {
    std::sort(transparent.begin(), transparent.end(),
              [](const TransparentDraw& a, const TransparentDraw& b) {
                return a.distance_sq > b.distance_sq;
              });

    // Transparency renders into a copy of the opaque result and refracts by
    // sampling the original, which never returns to attachment layout
    // afterwards: re-attaching a sampled image corrupts its compression
    // metadata on nvidia (the depth export exists for the same reason).
    ResourceHandle composite = graph_.CreateTexture(
        {.name = "composite", .format = kSceneColorFormat, .width = render_width_,
         .height = render_height_});
    lit = composite;
    graph_.AddPass(
        "opaque_copy",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Read(scene_color, ResourceUsage::kSampledCompute);
          builder.Write(composite, ResourceUsage::kStorageWrite);
        },
        [this, scene_color, composite](PassContext& ctx) {
          water_->RecordCopy(ctx, scene_color, composite, render_width_, render_height_);
        });

    ResourceHandle opaque_color = scene_color;
    ResourceHandle opaque_depth = depth_export;
    graph_.AddPass(
        "transparent",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Write(composite, ResourceUsage::kColorAttachment);
          builder.Write(motion, ResourceUsage::kColorAttachment);
          builder.Write(depth, ResourceUsage::kDepthAttachment);
          builder.Read(opaque_color, ResourceUsage::kSampledFragment);
          builder.Read(opaque_depth, ResourceUsage::kSampledFragment);
        },
        [this, composite, motion, depth, opaque_color, opaque_depth, tlas_slot, rt_shadows,
         ddgi_active, water_pipeline_active, transparent = std::move(transparent), &frame,
         &view](PassContext& ctx) {
          VkRenderingAttachmentInfo colors[2];
          colors[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          colors[0].imageView = ctx.graph->image(composite).view;
          colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          colors[1] = colors[0];
          colors[1].imageView = ctx.graph->image(motion).view;

          VkRenderingAttachmentInfo depth_attachment{
              .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          depth_attachment.imageView = ctx.graph->image(depth).view;
          depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
          depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

          VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
          rendering.renderArea = {{0, 0}, {render_width_, render_height_}};
          rendering.layerCount = 1;
          rendering.colorAttachmentCount = 2;
          rendering.pColorAttachments = colors;
          rendering.pDepthAttachment = &depth_attachment;
          vkCmdBeginRendering(ctx.cmd, &rendering);

          VkViewport viewport{0, 0, static_cast<f32>(render_width_),
                              static_cast<f32>(render_height_), 0.0f, 1.0f};
          VkRect2D scissor{{0, 0}, {render_width_, render_height_}};
          vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
          vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

          VkDescriptorSet globals_set = ctx.allocate_set(mesh_pipeline_->set_layout());
          VkDescriptorBufferInfo buffer_info{frame.globals.buffer, 0, sizeof(FrameGlobals)};
          VkWriteDescriptorSet writes[2];
          writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[0].dstSet = globals_set;
          writes[0].dstBinding = 0;
          writes[0].descriptorCount = 1;
          writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          writes[0].pBufferInfo = &buffer_info;
          u32 write_count = 1;
          VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
          VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
          if (rt_available_ && raytracing_) {
            tlas = raytracing_->tlas(tlas_slot);
            if (tlas != VK_NULL_HANDLE) {
              tlas_info.accelerationStructureCount = 1;
              tlas_info.pAccelerationStructures = &tlas;
              writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
              writes[1].pNext = &tlas_info;
              writes[1].dstSet = globals_set;
              writes[1].dstBinding = 1;
              writes[1].descriptorCount = 1;
              writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
              write_count = 2;
            }
          }
          vkUpdateDescriptorSets(device_->device(), write_count, writes, 0, nullptr);

          VkDescriptorSet env_set = ctx.allocate_set(environment_->env_set_layout());
          EnvironmentSystem::DdgiBinding ddgi_binding;
          if (ddgi_active) ddgi_binding = ddgi_->binding(frame_index_);
          environment_->WriteEnvSet(env_set, VK_NULL_HANDLE,
                                    ddgi_active ? &ddgi_binding : nullptr);

          enum class Mode { kNone, kWater, kBlend };
          Mode mode = Mode::kNone;
          VkDescriptorSet bound_material = VK_NULL_HANDLE;
          const DrawItem* bound_item = nullptr;
          for (const TransparentDraw& draw : transparent) {
            const GpuMesh* mesh = meshes_.find(draw.item->mesh);
            if (!mesh) continue;
            bool as_water = draw.submesh->water && water_pipeline_active;
  
            Mode wanted = as_water ? Mode::kWater : Mode::kBlend;
            if (mode != wanted) {
              if (as_water) {
                water_->Bind(ctx, globals_set, env_set, bindless_->set(), opaque_color,
                             opaque_depth);
              } else {
                mesh_pipeline_->BindBlend(ctx.cmd, globals_set, env_set, rt_shadows);
              }
              mode = wanted;
              bound_material = VK_NULL_HANDLE;
              bound_item = nullptr;
            }
            if (draw.item != bound_item) {
              MeshPushConstants push{.model = draw.item->transform,
                                     .prev_model = draw.item->prev_transform};
              if (as_water) {
                vkCmdPushConstants(ctx.cmd, water_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &mesh->vertices.buffer, &offset);
                vkCmdBindIndexBuffer(ctx.cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
              } else {
                mesh_pipeline_->Draw(ctx.cmd, *mesh, push);
              }
              bound_item = draw.item;
            }
            VkDescriptorSet material = material_system_->set(draw.submesh->material);
            if (material != bound_material) {
              if (as_water) {
                water_->BindMaterial(ctx.cmd, material);
              } else {
                mesh_pipeline_->BindMaterial(ctx.cmd, material);
              }
              bound_material = material;
            }
            mesh_pipeline_->DrawSubmesh(ctx.cmd, *draw.submesh);
          }
          vkCmdEndRendering(ctx.cmd);
        });
  }

  ResourceHandle post_input = lit;
  switch (settings_.aa_mode) {
    case AntiAliasingMode::kTaa:
      post_input = taa_.AddToGraph(graph_, lit, motion, frame_index_);
      break;
    case AntiAliasingMode::kUpscaler: {
      ResourceHandle upscaled =
          upscaler_->AddToGraph(graph_, {.color = lit,
                                         .depth = depth_export,
                                         .motion_vectors = motion,
                                         .jitter_x = jitter_x,
                                         .jitter_y = jitter_y,
                                         .sharpness = settings_.sharpness,
                                         .frame_delta_seconds = view.frame_delta_seconds,
                                         .camera_near = 0.1f,
                                         .camera_fov_y = view.camera.fov_y,
                                         .reset_history = first_frame});
      if (upscaled != kInvalidResource) post_input = upscaled;
      break;
    }
    case AntiAliasingMode::kNone:
      break;
  }

  // Dimensions of the aa-resolved image the post stack runs at.
  bool upscaled = settings_.aa_mode == AntiAliasingMode::kUpscaler && post_input != lit;
  u32 post_width = upscaled ? output_width_ : render_width_;
  u32 post_height = upscaled ? output_height_ : render_height_;

  exposure_.AddToGraph(graph_, post_input, post_width, post_height, view.frame_delta_seconds);
  ResourceHandle bloom = kInvalidResource;
  if (settings_.bloom) {
    bloom = bloom_.AddToGraph(graph_, post_input, post_width, post_height);
  }

  GpuImage backbuffer_image;
  backbuffer_image.image = swapchain_->image(image_index);
  backbuffer_image.view = swapchain_->view(image_index);
  backbuffer_image.format = swapchain_->format();
  backbuffer_image.extent = swapchain_->extent();
  ResourceHandle backbuffer = graph_.ImportBackbuffer(backbuffer_image);

  PostPass::Params post_params{static_cast<u32>(settings_.tonemap), settings_.bloom_intensity,
                               bloom != kInvalidResource ? 1u : 0u, 0};
  graph_.AddPass(
      "post",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(post_input, ResourceUsage::kSampledFragment);
        if (bloom != kInvalidResource) builder.Read(bloom, ResourceUsage::kSampledFragment);
        builder.Write(backbuffer, ResourceUsage::kColorAttachment);
      },
      [this, post_input, bloom, backbuffer, post_params](PassContext& ctx) {
        VkImageView bloom_view = bloom != kInvalidResource ? ctx.graph->image(bloom).view
                                                           : ctx.graph->image(post_input).view;
        post_->Record(ctx, ctx.graph->image(post_input).view, bloom_view,
                      exposure_.exposure_buffer(), exposure_.exposure_buffer_size(),
                      ctx.graph->image(backbuffer).view, ctx.graph->image(backbuffer).extent,
                      post_params);
      });

  if (view.ui_draw || view.hud_draw) {
    graph_.AddPass(
        "ui",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Write(backbuffer, ResourceUsage::kColorAttachment);
        },
        [this, backbuffer, &view](PassContext& ctx) {
          VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          color.imageView = ctx.graph->image(backbuffer).view;
          color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

          VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
          rendering.renderArea = {{0, 0}, ctx.graph->image(backbuffer).extent};
          rendering.layerCount = 1;
          rendering.colorAttachmentCount = 1;
          rendering.pColorAttachments = &color;
          vkCmdBeginRendering(ctx.cmd, &rendering);
          if (view.hud_draw) view.hud_draw(ctx.cmd);
          if (view.ui_draw) view.ui_draw(ctx.cmd);
          vkCmdEndRendering(ctx.cmd);
        });
  }
}

bool Renderer::CreateFrameResources() {
  for (FrameResources& frame : frames_) {
    VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = device_->graphics_family();
    if (vkCreateCommandPool(device_->device(), &pool_info, nullptr, &frame.pool) != VK_SUCCESS) {
      return false;
    }

    VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = frame.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_->device(), &alloc, &frame.cmd);

    VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(device_->device(), &semaphore_info, nullptr, &frame.image_available) !=
            VK_SUCCESS ||
        vkCreateFence(device_->device(), &fence_info, nullptr, &frame.in_flight) != VK_SUCCESS) {
      return false;
    }

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4},
    };
    VkDescriptorPoolCreateInfo descriptor_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_info.maxSets = 64;
    // The acceleration structure pool size is only legal with the extension
    // enabled, drop it otherwise.
    descriptor_info.poolSizeCount = device_->caps().raytracing ? 5 : 4;
    descriptor_info.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_->device(), &descriptor_info, nullptr,
                               &frame.descriptor_pool) != VK_SUCCESS) {
      return false;
    }

    frame.globals = device_->CreateBuffer(sizeof(FrameGlobals),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    if (!frame.globals.mapped) return false;

    // Bone palette: host visible, read in the skinned vertex shader through its
    // device address (no descriptor binding). Column-major 4x4 per bone.
    frame.bone_palette = device_->CreateBuffer(
        static_cast<u64>(kMaxFrameBones) * sizeof(Mat4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    if (!frame.bone_palette.mapped) return false;
    VkBufferDeviceAddressInfo address_info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = frame.bone_palette.buffer;
    frame.bone_palette_address = vkGetBufferDeviceAddress(device_->device(), &address_info);
  }
  return true;
}

void Renderer::DestroyFrameResources() {
  for (FrameResources& frame : frames_) {
    if (frame.globals.buffer) device_->DestroyBuffer(frame.globals);
    if (frame.bone_palette.buffer) device_->DestroyBuffer(frame.bone_palette);
    if (frame.descriptor_pool) {
      vkDestroyDescriptorPool(device_->device(), frame.descriptor_pool, nullptr);
    }
    if (frame.in_flight) vkDestroyFence(device_->device(), frame.in_flight, nullptr);
    if (frame.image_available) vkDestroySemaphore(device_->device(), frame.image_available, nullptr);
    if (frame.pool) vkDestroyCommandPool(device_->device(), frame.pool, nullptr);
    frame = {};
  }
  DestroyRenderFinishedSemaphores();
}

bool Renderer::CreateRenderFinishedSemaphores() {
  render_finished_.resize(swapchain_->image_count());
  for (VkSemaphore& semaphore : render_finished_) {
    VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device_->device(), &info, nullptr, &semaphore) != VK_SUCCESS) {
      return false;
    }
  }
  return true;
}

void Renderer::DestroyRenderFinishedSemaphores() {
  for (VkSemaphore semaphore : render_finished_) {
    if (semaphore) vkDestroySemaphore(device_->device(), semaphore, nullptr);
  }
  render_finished_.clear();
}

void Renderer::RecreateSwapchain() {
  u32 width = window_->width();
  u32 height = window_->height();
  if (width == 0 || height == 0) return;  // minimized
  device_->WaitIdle();
  swapchain_.reset();
  swapchain_ = Swapchain::Create(*device_, width, height, settings_.vsync);
  if (!swapchain_) return;
  DestroyRenderFinishedSemaphores();
  if (!CreateRenderFinishedSemaphores()) return;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  // The upscaler is sized for the output, rebuild it alongside.
  if (upscaler_) {
    upscaler_.reset();
    if (!CreateUpscalerForSettings()) {
      settings_.upscaler = UpscalerKind::kNone;
      settings_.aa_mode = AntiAliasingMode::kTaa;
      applied_upscaler_ = UpscalerKind::kNone;
    }
  }
  UpdateRenderResolution();
  transient_pool_->Clear();
  taa_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
  has_prev_frame_ = false;
}

void Renderer::WaitIdle() {
  if (device_ && !device_->is_stub()) device_->WaitIdle();
}

void Renderer::Shutdown() {
  if (device_ && !device_->is_stub()) {
    device_->WaitIdle();
    DestroyFrameResources();
    for (auto kv : meshes_) {
      device_->DestroyBuffer(kv.value.vertices);
      device_->DestroyBuffer(kv.value.indices);
      if (kv.value.skinning.buffer) device_->DestroyBuffer(kv.value.skinning);
    }
    meshes_.clear();
    taa_.Destroy(*device_);
    if (rt_available_) rtao_.Destroy(*device_);
    bloom_.Destroy(*device_);
    exposure_.Destroy(*device_);
    water_.reset();
    ddgi_.reset();
    environment_.reset();
    material_system_.reset();
    bindless_.reset();
    transient_pool_.reset();
  }
  graph_.Reset();
  post_.reset();
  mesh_pipeline_.reset();
  swapchain_.reset();
  upscaler_.reset();
  raytracing_.reset();
  device_.reset();
}

const DeviceCaps* Renderer::caps() const { return device_ ? &device_->caps() : nullptr; }

VkFormat Renderer::swapchain_format() const {
  return swapchain_ ? swapchain_->format() : VK_FORMAT_UNDEFINED;
}

u32 Renderer::swapchain_image_count() const {
  return swapchain_ ? swapchain_->image_count() : 0;
}

}  // namespace rec::render

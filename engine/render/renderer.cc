#include "render/renderer.h"

#include <cmath>
#include <cstring>

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
  material_system_ = MaterialSystem::Create(*device_);
  if (!material_system_) return false;
  mesh_pipeline_ = MeshPipeline::Create(*device_, kSceneColorFormat, kMotionFormat, kDepthFormat,
                                        material_system_->set_layout());
  post_ = PostPass::Create(*device_, swapchain_->format());
  if (!mesh_pipeline_ || !post_ || !taa_.Initialize(*device_)) return false;

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

  return true;
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
    }
    taa_.Reset();
    has_prev_frame_ = false;
  }

  taa_.Configure({.history_blend = settings_.taa_history_blend,
                  .jitter_sample_count = taa_.settings().jitter_sample_count});
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
  if (lod.submeshes.empty()) {
    gpu.submeshes.push_back({0, gpu.index_count, 0});
  } else {
    for (const asset::Submesh& submesh : lod.submeshes) {
      gpu.submeshes.push_back({submesh.index_offset, submesh.index_count, submesh.material.hash});
    }
  }
  meshes_[mesh.id.hash] = gpu;
  if (raytracing_) raytracing_->BuildBlas(mesh.id.hash, gpu);
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

  VkResult presented = swapchain_->Present(render_finished_[image_index], image_index);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
    RecreateSwapchain();
  }
  ++frame_index_;
}

void Renderer::BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view) {
  bool rt_shadows = rt_available_ && settings_.rt_shadows;

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
  std::memcpy(frame.globals.mapped, &globals, sizeof(globals));
  prev_view_proj_ = view_proj;
  has_prev_frame_ = true;

  ResourceHandle scene_color = graph_.CreateTexture(
      {.name = "scene_color", .format = kSceneColorFormat, .width = render_width_,
       .height = render_height_});
  ResourceHandle motion = graph_.CreateTexture(
      {.name = "motion", .format = kMotionFormat, .width = render_width_,
       .height = render_height_});
  ResourceHandle depth = graph_.CreateTexture(
      {.name = "depth", .format = kDepthFormat, .width = render_width_,
       .height = render_height_});

  u32 tlas_slot = frame_index_ % RayTracingContext::kSlots;
  if (rt_shadows) {
    base::Vector<RayTracingContext::Instance> instances;
    instances.reserve(view.draws.size());
    for (const DrawItem& item : view.draws) {
      instances.push_back({.mesh_key = item.mesh, .transform = item.transform});
    }
    graph_.AddPass(
        "tlas_build", [](RenderGraph::PassBuilder&) {},
        [this, tlas_slot, instances = std::move(instances)](PassContext& ctx) {
          raytracing_->BuildTlas(ctx.cmd, tlas_slot, instances);
        });
  }

  graph_.AddPass(
      "scene",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(scene_color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, scene_color, motion, depth, tlas_slot, rt_shadows, &frame,
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
        colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

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

        mesh_pipeline_->Bind(ctx.cmd, globals_set, rt_shadows, settings_.wireframe);
        VkDescriptorSet bound_material = VK_NULL_HANDLE;
        for (const DrawItem& item : view.draws) {
          const GpuMesh* mesh = meshes_.find(item.mesh);
          if (!mesh) continue;
          mesh_pipeline_->Draw(ctx.cmd, *mesh,
                               {.model = item.transform, .prev_model = item.prev_transform});
          for (const GpuSubmesh& submesh : mesh->submeshes) {
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

  ResourceHandle post_input = scene_color;
  switch (settings_.aa_mode) {
    case AntiAliasingMode::kTaa:
      post_input = taa_.AddToGraph(graph_, scene_color, motion, frame_index_);
      break;
    case AntiAliasingMode::kUpscaler: {
      ResourceHandle upscaled =
          upscaler_->AddToGraph(graph_, {.color = scene_color,
                                         .depth = depth,
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

  GpuImage backbuffer_image;
  backbuffer_image.image = swapchain_->image(image_index);
  backbuffer_image.view = swapchain_->view(image_index);
  backbuffer_image.format = swapchain_->format();
  backbuffer_image.extent = swapchain_->extent();
  ResourceHandle backbuffer = graph_.ImportBackbuffer(backbuffer_image);

  PostPass::Params post_params{settings_.exposure, static_cast<u32>(settings_.tonemap)};
  graph_.AddPass(
      "post",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(post_input, ResourceUsage::kSampledFragment);
        builder.Write(backbuffer, ResourceUsage::kColorAttachment);
      },
      [this, post_input, backbuffer, post_params](PassContext& ctx) {
        post_->Record(ctx, ctx.graph->image(post_input).view, ctx.graph->image(backbuffer).view,
                      ctx.graph->image(backbuffer).extent, post_params);
      });

  if (view.ui_draw) {
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
          view.ui_draw(ctx.cmd);
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
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4},
    };
    VkDescriptorPoolCreateInfo descriptor_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_info.maxSets = 16;
    // The acceleration structure pool size is only legal with the extension
    // enabled, drop it otherwise.
    descriptor_info.poolSizeCount = device_->caps().raytracing ? 4 : 3;
    descriptor_info.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_->device(), &descriptor_info, nullptr,
                               &frame.descriptor_pool) != VK_SUCCESS) {
      return false;
    }

    frame.globals = device_->CreateBuffer(sizeof(FrameGlobals),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    if (!frame.globals.mapped) return false;
  }
  return true;
}

void Renderer::DestroyFrameResources() {
  for (FrameResources& frame : frames_) {
    if (frame.globals.buffer) device_->DestroyBuffer(frame.globals);
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
    }
    meshes_.clear();
    taa_.Destroy(*device_);
    material_system_.reset();
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

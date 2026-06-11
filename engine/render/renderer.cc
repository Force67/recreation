#include "render/renderer.h"

#include <cstring>

#include "core/log.h"

namespace rec::render {

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Initialize(const RendererDesc& desc, Window& window) {
  desc_ = desc;
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

  swapchain_ = Swapchain::Create(*device_, output_width_, output_height_);
  if (!swapchain_ || !CreateFrameResources()) return false;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  transient_pool_ = std::make_unique<TransientPool>(*device_);
  mesh_pipeline_ =
      MeshPipeline::Create(*device_, kSceneColorFormat, kMotionFormat, kDepthFormat);
  post_ = PostPass::Create(*device_, swapchain_->format());
  if (!mesh_pipeline_ || !post_ || !taa_.Initialize(*device_)) return false;

  if (desc.upscaler != UpscalerKind::kNone) {
    upscaler_ = CreateUpscaler({.kind = desc.upscaler,
                                .render_width = output_width_ * 2 / 3,
                                .render_height = output_height_ * 2 / 3,
                                .output_width = output_width_,
                                .output_height = output_height_},
                               *device_);
    if (upscaler_) {
      desc_.aa_mode = AntiAliasingMode::kUpscaler;
    } else {
      REC_WARN("upscaler unavailable, falling back to taa");
      desc_.upscaler = UpscalerKind::kNone;
      desc_.aa_mode = AntiAliasingMode::kTaa;
    }
  }
  UpdateRenderResolution();
  taa_.Resize(*device_, {render_width_, render_height_});

  if (desc.enable_raytracing && device_->caps().raytracing) {
    raytracing_ = std::make_unique<RayTracingContext>(*device_);
    raytracing_->Configure(desc.raytracing);
  }

  return true;
}

void Renderer::UpdateRenderResolution() {
  if (upscaler_) {
    // Quality preset, 1.5x per axis. Presets become configurable later.
    render_width_ = output_width_ * 2 / 3;
    render_height_ = output_height_ * 2 / 3;
  } else {
    render_width_ = output_width_;
    render_height_ = output_height_;
  }
}

bool Renderer::UploadMesh(const asset::Mesh& mesh) {
  if (!device_ || device_->is_stub()) return false;
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) return false;

  const asset::MeshLod& lod = mesh.lods[0];
  GpuMesh gpu;
  gpu.vertices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.vertices.data()),
               lod.vertices.size() * sizeof(asset::Vertex)),
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  gpu.indices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.indices.data()), lod.indices.size() * sizeof(u32)),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  gpu.index_count = static_cast<u32>(lod.indices.size());
  meshes_[mesh.id.hash] = gpu;
  return true;
}

void Renderer::RenderFrame(const FrameView& view) {
  if (!device_ || device_->is_stub() || !swapchain_) return;

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
  signal.semaphore = frame.render_finished;
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

  VkResult presented = swapchain_->Present(frame.render_finished, image_index);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
    RecreateSwapchain();
  }
  ++frame_index_;
}

void Renderer::BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view) {
  // Camera state for both this frame and reprojection. Jitter lives in the
  // projection, not the matrices used for motion vectors.
  f32 aspect = static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
  Mat4 proj = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f);
  Mat4 view_proj = proj * LookAt(view.camera.eye, view.camera.target, {0, 1, 0});

  bool temporal =
      desc_.aa_mode == AntiAliasingMode::kTaa || desc_.aa_mode == AntiAliasingMode::kUpscaler;
  f32 jitter_x = 0, jitter_y = 0;
  if (temporal) {
    JitterSequence::Sample(frame_index_, taa_.settings().jitter_sample_count, &jitter_x,
                           &jitter_y);
  }

  FrameGlobals globals;
  globals.view_proj = view_proj;
  globals.prev_view_proj = has_prev_frame_ ? prev_view_proj_ : view_proj;
  globals.jitter[0] = 2.0f * jitter_x / static_cast<f32>(render_width_);
  globals.jitter[1] = 2.0f * jitter_y / static_cast<f32>(render_height_);
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

  graph_.AddPass(
      "scene",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(scene_color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, scene_color, motion, depth, &frame, &view](PassContext& ctx) {
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
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = globals_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);

        mesh_pipeline_->Bind(ctx.cmd, globals_set);
        for (const DrawItem& item : view.draws) {
          auto it = meshes_.find(item.mesh);
          if (it == meshes_.end()) continue;
          mesh_pipeline_->Draw(ctx.cmd, it->second,
                               {.model = item.transform, .prev_model = item.prev_transform});
        }
        vkCmdEndRendering(ctx.cmd);
      });

  if (raytracing_) raytracing_->AddPasses(graph_);

  ResourceHandle post_input = scene_color;
  switch (desc_.aa_mode) {
    case AntiAliasingMode::kTaa:
      post_input = taa_.AddToGraph(graph_, scene_color, motion, frame_index_);
      break;
    case AntiAliasingMode::kUpscaler:
      upscaler_->AddToGraph(graph_, {.color = scene_color,
                                     .depth = depth,
                                     .motion_vectors = motion,
                                     .jitter_x = jitter_x,
                                     .jitter_y = jitter_y});
      break;
    case AntiAliasingMode::kNone:
      break;
  }

  GpuImage backbuffer_image;
  backbuffer_image.image = swapchain_->image(image_index);
  backbuffer_image.view = swapchain_->view(image_index);
  backbuffer_image.format = swapchain_->format();
  backbuffer_image.extent = swapchain_->extent();
  ResourceHandle backbuffer = graph_.ImportBackbuffer(backbuffer_image);

  graph_.AddPass(
      "post",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(post_input, ResourceUsage::kSampledFragment);
        builder.Write(backbuffer, ResourceUsage::kColorAttachment);
      },
      [this, post_input, backbuffer](PassContext& ctx) {
        post_->Record(ctx, ctx.graph->image(post_input).view, ctx.graph->image(backbuffer).view,
                      ctx.graph->image(backbuffer).extent);
      });
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
        vkCreateSemaphore(device_->device(), &semaphore_info, nullptr, &frame.render_finished) !=
            VK_SUCCESS ||
        vkCreateFence(device_->device(), &fence_info, nullptr, &frame.in_flight) != VK_SUCCESS) {
      return false;
    }

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
    };
    VkDescriptorPoolCreateInfo descriptor_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_info.maxSets = 16;
    descriptor_info.poolSizeCount = 3;
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
    if (frame.render_finished) vkDestroySemaphore(device_->device(), frame.render_finished, nullptr);
    if (frame.pool) vkDestroyCommandPool(device_->device(), frame.pool, nullptr);
    frame = {};
  }
}

void Renderer::RecreateSwapchain() {
  u32 width = window_->width();
  u32 height = window_->height();
  if (width == 0 || height == 0) return;  // minimized
  device_->WaitIdle();
  swapchain_.reset();
  swapchain_ = Swapchain::Create(*device_, width, height);
  if (!swapchain_) return;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;
  UpdateRenderResolution();
  transient_pool_->Clear();
  taa_.Resize(*device_, {render_width_, render_height_});
  has_prev_frame_ = false;
}

void Renderer::Shutdown() {
  if (device_ && !device_->is_stub()) {
    device_->WaitIdle();
    DestroyFrameResources();
    for (auto& [key, mesh] : meshes_) {
      device_->DestroyBuffer(mesh.vertices);
      device_->DestroyBuffer(mesh.indices);
    }
    meshes_.clear();
    taa_.Destroy(*device_);
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

void Renderer::SetAntiAliasing(AntiAliasingMode mode) {
  if (mode == AntiAliasingMode::kUpscaler && !upscaler_) mode = AntiAliasingMode::kTaa;
  desc_.aa_mode = mode;
  taa_.Reset();
}

void Renderer::SetUpscaler(UpscalerKind kind) {
  desc_.upscaler = kind;
  taa_.Reset();
  // TODO: recreate upscaler and transient targets at the new render resolution.
}

const DeviceCaps* Renderer::caps() const { return device_ ? &device_->caps() : nullptr; }

}  // namespace rec::render

#include "render/renderer.h"

#include "core/log.h"

namespace rec::render {

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Initialize(const RendererDesc& desc, Window& window) {
  desc_ = desc;
  output_width_ = window.width();
  output_height_ = window.height();
  render_width_ = output_width_;
  render_height_ = output_height_;

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

  if (desc.upscaler != UpscalerKind::kNone) {
    // Quality preset, 1.5x per axis. Presets become configurable later.
    render_width_ = output_width_ * 2 / 3;
    render_height_ = output_height_ * 2 / 3;
    upscaler_ = CreateUpscaler({.kind = desc.upscaler,
                                .render_width = render_width_,
                                .render_height = render_height_,
                                .output_width = output_width_,
                                .output_height = output_height_},
                               *device_);
    if (upscaler_) {
      desc_.aa_mode = AntiAliasingMode::kUpscaler;
    } else {
      REC_WARN("upscaler unavailable, falling back to taa");
      desc_.aa_mode = AntiAliasingMode::kTaa;
      render_width_ = output_width_;
      render_height_ = output_height_;
    }
  }

  if (desc.enable_raytracing && device_->caps().raytracing) {
    raytracing_ = std::make_unique<RayTracingContext>(*device_);
    raytracing_->Configure(desc.raytracing);
  }

  return true;
}

void Renderer::RenderFrame(ecs::World& world, f32 interpolation_alpha) {
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

  vkResetFences(device_->device(), 1, &frame.in_flight);
  vkResetCommandPool(device_->device(), frame.pool, 0);

  graph_.Reset();
  BuildFrameGraph();
  graph_.Compile();
  graph_.Execute();

  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.cmd, &begin);
  RecordFrame(frame.cmd, image_index);
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

void Renderer::RecordFrame(VkCommandBuffer cmd, u32 image_index) {
  VkImageMemoryBarrier2 to_color{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  to_color.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_color.image = swapchain_->image(image_index);
  to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &to_color;
  vkCmdPipelineBarrier2(cmd, &dep);

  VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = swapchain_->view(image_index);
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{0.02f, 0.02f, 0.05f, 1.0f}};

  VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, swapchain_->extent()};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;

  vkCmdBeginRendering(cmd, &rendering);
  // TODO: graph passes record here once the rhi grows pipelines and the
  // graph compiles to barriers instead of executing callbacks directly.
  vkCmdEndRendering(cmd);

  VkImageMemoryBarrier2 to_present = to_color;
  to_present.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_present.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
  to_present.dstAccessMask = 0;
  to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  dep.pImageMemoryBarriers = &to_present;
  vkCmdPipelineBarrier2(cmd, &dep);
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
  }
  return true;
}

void Renderer::DestroyFrameResources() {
  for (FrameResources& frame : frames_) {
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
  output_width_ = width;
  output_height_ = height;
  taa_.Reset();
}

void Renderer::Shutdown() {
  if (device_ && !device_->is_stub()) {
    device_->WaitIdle();
    DestroyFrameResources();
  }
  swapchain_.reset();
  upscaler_.reset();
  raytracing_.reset();
  device_.reset();
}

void Renderer::SetAntiAliasing(AntiAliasingMode mode) {
  desc_.aa_mode = mode;
  taa_.Reset();
}

void Renderer::SetUpscaler(UpscalerKind kind) {
  desc_.upscaler = kind;
  taa_.Reset();
  // TODO: recreate upscaler and transient targets at the new render resolution.
}

const DeviceCaps* Renderer::caps() const { return device_ ? &device_->caps() : nullptr; }

void Renderer::BuildFrameGraph() {
  auto color = graph_.CreateTexture(
      {.name = "scene_color", .width = render_width_, .height = render_height_});
  auto depth = graph_.CreateTexture({.name = "depth",
                                     .format = ResourceFormat::kDepth32Float,
                                     .width = render_width_,
                                     .height = render_height_});
  auto motion = graph_.CreateTexture({.name = "motion_vectors",
                                      .format = ResourceFormat::kRg16Float,
                                      .width = render_width_,
                                      .height = render_height_});

  graph_.AddPass(
      "gbuffer",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color);
        builder.Write(depth);
        builder.Write(motion);
      },
      [] {});

  if (raytracing_) raytracing_->AddPasses(graph_);

  switch (desc_.aa_mode) {
    case AntiAliasingMode::kTaa:
      taa_.AddToGraph(graph_, frame_index_);
      break;
    case AntiAliasingMode::kUpscaler: {
      f32 jitter_x = 0, jitter_y = 0;
      JitterSequence::Sample(frame_index_, 16, &jitter_x, &jitter_y);
      upscaler_->AddToGraph(graph_, {.color = color,
                                     .depth = depth,
                                     .motion_vectors = motion,
                                     .jitter_x = jitter_x,
                                     .jitter_y = jitter_y});
      break;
    }
    case AntiAliasingMode::kNone:
      break;
  }

  auto backbuffer = graph_.ImportBackbuffer();
  graph_.AddPass(
      "present",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color);
        builder.Write(backbuffer);
      },
      [] {});
}

}  // namespace rec::render

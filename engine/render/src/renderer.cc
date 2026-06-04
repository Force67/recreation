#include "recreation/render/renderer.h"

#include "recreation/core/log.h"

namespace rec::render {

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Initialize(const RendererDesc& desc, Window& window) {
  desc_ = desc;
  output_width_ = window.width();
  output_height_ = window.height();
  render_width_ = output_width_;
  render_height_ = output_height_;

  device_ = Device::Create({.enable_validation = desc.enable_validation,
                            .request_raytracing = desc.enable_raytracing},
                           window.native_handles());
  if (device_->is_stub()) {
    REC_WARN("renderer running in stub mode");
    return true;
  }

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
  if (!device_ || device_->is_stub()) return;

  graph_.Reset();
  BuildFrameGraph();
  graph_.Compile();
  graph_.Execute();
  ++frame_index_;
}

void Renderer::Shutdown() {
  if (device_) device_->WaitIdle();
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

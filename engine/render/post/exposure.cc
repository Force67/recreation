#include "render/post/exposure.h"

#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/exposure_resolve_cs_hlsl.h"
#include "shaders/histogram_cs_hlsl.h"

namespace rec::render {
namespace {

// Metering range in log2 luminance; bin 0 stays reserved for pure black.
constexpr f32 kMinLogLuma = -10.0f;
constexpr f32 kMaxLogLuma = 14.0f;

struct HistogramPush {
  f32 min_log_luma;
  f32 inv_log_luma_range;
  u32 width;
  u32 height;
};

struct ResolvePush {
  f32 min_log_luma;
  f32 log_luma_range;
  f32 delta_seconds;
  f32 adaptation_speed;
  f32 compensation;
  u32 auto_exposure;
  f32 manual_exposure;
  f32 pixel_count;
  f32 low_percentile;
  f32 high_percentile;
};

}  // namespace

bool ExposurePass::Initialize(Device& device) {
  device_ = &device;
  sampler_ = device.GetSampler({.min_filter = Filter::kNearest, .mag_filter = Filter::kNearest});

  histogram_ =
      device.CreateBuffer(256 * sizeof(u32), kBufferUsageStorage | kBufferUsageTransferDst);
  exposure_ =
      device.CreateBuffer(2 * sizeof(f32), kBufferUsageStorage | kBufferUsageTransferDst);
  if (!histogram_ || !exposure_) return false;

  histogram_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_histogram_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(HistogramPush),
      .debug_name = "exposure_histogram",
  });
  resolve_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_exposure_resolve_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(ResolvePush),
      .debug_name = "exposure_resolve",
  });
  if (!histogram_pipeline_ || !resolve_pipeline_) {
    REC_ERROR("exposure pipeline creation failed");
    return false;
  }
  return true;
}

void ExposurePass::Destroy(Device& device) {
  device.DestroyBuffer(histogram_);
  device.DestroyBuffer(exposure_);
  device.DestroyPipeline(histogram_pipeline_);
  device.DestroyPipeline(resolve_pipeline_);
  histogram_pipeline_ = {};
  resolve_pipeline_ = {};
  sampler_ = {};
}

void ExposurePass::AddToGraph(RenderGraph& graph, ResourceHandle input, u32 width, u32 height,
                              f32 delta_seconds) {
  bool first = first_frame_;
  first_frame_ = false;

  graph.AddPass(
      "exposure",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(input, ResourceUsage::kSampledCompute);
      },
      [this, input, width, height, delta_seconds, first](PassContext& ctx) {
        if (first) {
          // Known starting state for both buffers.
          ctx.cmd->FillBuffer(histogram_, 0, histogram_.size, 0);
          ctx.cmd->FillBuffer(exposure_, 0, exposure_.size, 0);
        }
        // First frame: fills -> compute. Steady state: last frame's compute
        // writes + the tonemap fragment read -> this frame's compute.
        auto buffer_barrier = [&] {
          if (first) {
            ctx.cmd->MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kComputeRead);
          } else {
            ctx.cmd->MemoryBarrier(BarrierScope::kAllCommands, BarrierScope::kComputeRead);
          }
        };
        buffer_barrier();

        // Histogram.
        HistogramPush histogram_push{};
        histogram_push.min_log_luma = kMinLogLuma;
        histogram_push.inv_log_luma_range = 1.0f / (kMaxLogLuma - kMinLogLuma);
        histogram_push.width = width;
        histogram_push.height = height;
        ctx.cmd->BindPipeline(histogram_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Combined(0, ctx.graph->image(input).view, sampler_),
                Bind::StorageBuffer(1, histogram_, 0, histogram_.size)});
        ctx.cmd->Push(histogram_push);
        ctx.cmd->Dispatch2D({width, height}, 16);

        buffer_barrier();

        // Resolve + adaptation.
        ResolvePush resolve_push{};
        resolve_push.min_log_luma = kMinLogLuma;
        resolve_push.log_luma_range = kMaxLogLuma - kMinLogLuma;
        resolve_push.delta_seconds = delta_seconds;
        resolve_push.adaptation_speed = settings_.adaptation_speed;
        resolve_push.compensation = settings_.compensation;
        resolve_push.auto_exposure = settings_.automatic ? 1u : 0u;
        resolve_push.manual_exposure = settings_.manual_exposure;
        resolve_push.pixel_count = static_cast<f32>(width) * static_cast<f32>(height);
        // Meter the 60th..97th percentile of the weighted mass: dark corners
        // and blown highlights (sun disc, sky) stop steering the exposure.
        resolve_push.low_percentile = 0.6f;
        resolve_push.high_percentile = 0.97f;
        ctx.cmd->BindPipeline(resolve_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, histogram_, 0, histogram_.size),
                                   Bind::StorageBuffer(1, exposure_, 0, exposure_.size)});
        ctx.cmd->Push(resolve_push);
        ctx.cmd->Dispatch(1, 1, 1);

        // Visible to the tonemap fragment shader.
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });
}

}  // namespace rec::render

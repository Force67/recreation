#include "render/geometry/hair_strands.h"

#include <cmath>
#include <vector>

#include "core/log.h"
#include "shaders/hair_ps_hlsl.h"
#include "shaders/hair_sim_cs_hlsl.h"
#include "shaders/hair_vs_hlsl.h"

namespace rec::render {
namespace {

struct HairPoint {
  f32 pos[4];   // xyz, w inv_mass
  f32 prev[4];  // xyz previous, w rest length
};

struct SimPush {
  f32 head[4];
  f32 wind[4];
  u32 strand_count;
  u32 points_per_strand;
  f32 dt;
  f32 damping;
};

struct DrawPush {
  Mat4 view_proj;
  f32 camera[4];
  f32 sun[4];
  f32 sun_color[4];
  u32 points_per_strand;
  f32 width;
  f32 pad0;
  f32 pad1;
};

ByteSpan Span(const void* data, size_t bytes) {
  return ByteSpan(static_cast<const u8*>(data), bytes);
}

}  // namespace

bool HairStrands::Initialize(Device& device, Format color_format, Format depth_format) {
  sim_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_hair_sim_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(SimPush),
      .debug_name = "hair_sim",
  });
  draw_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_hair_vs_hlsl),
      .fragment = REC_SHADER(k_hair_ps_hlsl),
      .raster = {.cull = CullMode::kNone},  // ribbons flip with the view
      .depth = {.test = true, .write = true, .compare = CompareOp::kGreaterEqual,
                .format = depth_format},
      .color_formats = {color_format},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}}, .stages = kShaderStageVertex}},
      .push_constant_size = sizeof(DrawPush),
      .debug_name = "hair_draw",
  });
  if (!sim_pipeline_ || !draw_pipeline_) {
    REC_ERROR("hair pipeline creation failed");
    return false;
  }
  return true;
}

void HairStrands::Destroy(Device& device) {
  for (PipelineHandle* p : {&sim_pipeline_, &draw_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  if (points_) device.DestroyBuffer(points_);
  points_ = {};
  if (indices_) device.DestroyBuffer(indices_);
  indices_ = {};
}

void HairStrands::SeedCap(Device& device, const Vec3& head_center, f32 head_radius,
                          u32 strand_count, f32 strand_length) {
  head_center_ = head_center;
  head_radius_ = head_radius;
  strand_count_ = strand_count;

  // Fibonacci-distributed roots over the upper hemisphere, strands initially
  // pointing along the surface normal; the sim relaxes them under gravity.
  std::vector<HairPoint> points;
  points.reserve(static_cast<size_t>(strand_count) * kPointsPerStrand);
  const f32 golden = 2.399963f;
  const f32 segment = strand_length / (kPointsPerStrand - 1);
  for (u32 s = 0; s < strand_count; ++s) {
    f32 t = (static_cast<f32>(s) + 0.5f) / strand_count;
    f32 y = 0.45f + 0.55f * t;  // crown + upper sides only
    f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    f32 a = golden * static_cast<f32>(s);
    Vec3 n{r * std::cos(a), y, r * std::sin(a)};
    Vec3 root = head_center + n * head_radius;
    for (u32 i = 0; i < kPointsPerStrand; ++i) {
      Vec3 p = root + n * (segment * static_cast<f32>(i));
      HairPoint hp{};
      hp.pos[0] = p.x;
      hp.pos[1] = p.y;
      hp.pos[2] = p.z;
      hp.pos[3] = i == 0 ? 0.0f : 1.0f;  // root pinned
      hp.prev[0] = p.x;
      hp.prev[1] = p.y;
      hp.prev[2] = p.z;
      hp.prev[3] = i == 0 ? 0.0f : segment;
      points.push_back(hp);
    }
  }

  // Ribbon topology: per segment, two triangles over the (point, side) grid.
  std::vector<u32> idx;
  idx.reserve(static_cast<size_t>(strand_count) * (kPointsPerStrand - 1) * 6);
  for (u32 s = 0; s < strand_count; ++s) {
    u32 base = s * kPointsPerStrand * 2;
    for (u32 i = 0; i + 1 < kPointsPerStrand; ++i) {
      u32 v0 = base + i * 2, v1 = v0 + 1, v2 = v0 + 2, v3 = v0 + 3;
      idx.push_back(v0);
      idx.push_back(v2);
      idx.push_back(v1);
      idx.push_back(v1);
      idx.push_back(v2);
      idx.push_back(v3);
    }
  }
  index_count_ = static_cast<u32>(idx.size());

  if (points_) device.DestroyBuffer(points_);
  if (indices_) device.DestroyBuffer(indices_);
  points_ = device.CreateBufferWithData(Span(points.data(), points.size() * sizeof(HairPoint)),
                                        kBufferUsageStorage);
  indices_ = device.CreateBufferWithData(Span(idx.data(), idx.size() * sizeof(u32)),
                                         kBufferUsageIndex);
  REC_INFO("hair: {} strands, {} points, {} ribbon tris", strand_count_,
           points.size(), index_count_ / 3);
}

void HairStrands::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                             Extent2D extent, const Frame& frame) {
  if (!active()) return;

  graph.AddPass(
      "hair_sim", [](RenderGraph::PassBuilder&) {},
      [this, frame](PassContext& ctx) {
        SimPush push{};
        push.head[0] = head_center_.x;
        push.head[1] = head_center_.y;
        push.head[2] = head_center_.z;
        push.head[3] = head_radius_ * 1.02f;
        push.wind[0] = frame.wind[0];
        push.wind[1] = frame.wind[1];
        push.wind[2] = frame.wind[2];
        push.wind[3] = frame.time;
        push.strand_count = strand_count_;
        push.points_per_strand = kPointsPerStrand;
        push.dt = frame.delta_seconds;
        push.damping = 0.985f;
        ctx.cmd->BindPipeline(sim_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, points_, 0, points_.size)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch((strand_count_ + 63) / 64, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });

  graph.AddPass(
      "hair_draw",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(color, ResourceUsage::kColorAttachment);
        b.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, color, depth, extent, frame](PassContext& ctx) {
        DrawPush push{};
        push.view_proj = frame.view_proj;
        push.camera[0] = frame.camera_pos.x;
        push.camera[1] = frame.camera_pos.y;
        push.camera[2] = frame.camera_pos.z;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun[0] = sun.x;
        push.sun[1] = sun.y;
        push.sun[2] = sun.z;
        push.sun[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.points_per_strand = kPointsPerStrand;
        push.width = 0.0011f;

        ColorAttachment att{.view = ctx.graph->image(color).view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering(
            {.extent = extent, .colors = {&att, 1}, .depth = &depth_att});
        ctx.cmd->BindPipeline(draw_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, points_, 0, points_.size)});
        ctx.cmd->Push(push);
        ctx.cmd->BindIndexBuffer(indices_, 0, IndexType::kUint32);
        ctx.cmd->DrawIndexed(index_count_, 1, 0, 0, 0);
        ctx.cmd->EndRendering();
      });
}

}  // namespace rec::render

#include "recreation/render/antialiasing.h"

#include "recreation/render/render_graph.h"

namespace rec::render {
namespace {

f32 Halton(u32 index, u32 base) {
  f32 result = 0.0f;
  f32 fraction = 1.0f;
  for (u32 i = index + 1; i > 0; i /= base) {
    fraction /= static_cast<f32>(base);
    result += fraction * static_cast<f32>(i % base);
  }
  return result;
}

}  // namespace

void JitterSequence::Sample(u32 frame_index, u32 sample_count, f32* out_x, f32* out_y) {
  u32 index = frame_index % sample_count;
  *out_x = Halton(index, 2) - 0.5f;
  *out_y = Halton(index, 3) - 0.5f;
}

void TaaPass::Reset() { history_valid_ = false; }

void TaaPass::AddToGraph(RenderGraph& graph, u32 frame_index) {
  graph.AddPass(
      "taa_resolve",
      [](RenderGraph::PassBuilder&) {
        // TODO: read scene color, depth, motion vectors and history,
        // write the resolved target and next frame's history.
      },
      [this] { history_valid_ = true; });
}

}  // namespace rec::render

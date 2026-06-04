#include "recreation/render/raytracing.h"

#include "recreation/render/render_graph.h"

namespace rec::render {

void RayTracingContext::BuildAccelerationStructures() {
  // TODO: BLAS per mesh, TLAS from visible instances.
}

void RayTracingContext::AddPasses(RenderGraph& graph) {
  if (settings_.shadows) {
    graph.AddPass("rt_shadows", [](RenderGraph::PassBuilder&) {}, [] {});
  }
  if (settings_.reflections) {
    graph.AddPass("rt_reflections", [](RenderGraph::PassBuilder&) {}, [] {});
  }
}

}  // namespace rec::render

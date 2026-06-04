#include "recreation/render/render_graph.h"

namespace rec::render {

ResourceHandle RenderGraph::CreateTexture(const TransientTextureDesc& desc) {
  textures_.push_back(desc);
  return static_cast<ResourceHandle>(textures_.size());
}

ResourceHandle RenderGraph::ImportBackbuffer() {
  textures_.push_back({.name = "backbuffer"});
  return static_cast<ResourceHandle>(textures_.size());
}

void RenderGraph::AddPass(std::string name, SetupFn setup, ExecuteFn execute) {
  Pass pass{.name = std::move(name), .builder = {}, .execute = std::move(execute)};
  setup(pass.builder);
  passes_.push_back(std::move(pass));
}

void RenderGraph::Compile() {
  // TODO: cull unreferenced passes, derive barriers from read/write sets,
  // alias transient memory.
}

void RenderGraph::Execute() {
  for (auto& pass : passes_) {
    if (pass.execute) pass.execute();
  }
}

void RenderGraph::Reset() {
  textures_.clear();
  passes_.clear();
}

}  // namespace rec::render

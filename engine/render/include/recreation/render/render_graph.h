#ifndef RECREATION_RENDER_RENDER_GRAPH_H_
#define RECREATION_RENDER_RENDER_GRAPH_H_

#include <functional>
#include <string>
#include <vector>

#include "recreation/core/types.h"

namespace rec::render {

using ResourceHandle = u32;
constexpr ResourceHandle kInvalidResource = 0;

enum class ResourceFormat : u8 {
  kRgba8Unorm,
  kRgba16Float,
  kRg16Float,
  kDepth32Float,
};

struct TransientTextureDesc {
  std::string name;
  ResourceFormat format = ResourceFormat::kRgba16Float;
  u32 width = 0;
  u32 height = 0;
};

// Declared per frame, compiled into barriers and executed. Pass setup
// declares reads/writes, execution records into a command buffer. Compile is
// a stub until the RHI grows command buffers, but the API is what passes
// program against from day one.
class RenderGraph {
 public:
  struct PassBuilder {
    void Read(ResourceHandle handle) { reads.push_back(handle); }
    void Write(ResourceHandle handle) { writes.push_back(handle); }
    std::vector<ResourceHandle> reads;
    std::vector<ResourceHandle> writes;
  };

  using SetupFn = std::function<void(PassBuilder&)>;
  using ExecuteFn = std::function<void()>;

  ResourceHandle CreateTexture(const TransientTextureDesc& desc);
  ResourceHandle ImportBackbuffer();

  void AddPass(std::string name, SetupFn setup, ExecuteFn execute);

  void Compile();
  void Execute();
  void Reset();

 private:
  struct Pass {
    std::string name;
    PassBuilder builder;
    ExecuteFn execute;
  };

  std::vector<TransientTextureDesc> textures_;
  std::vector<Pass> passes_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDER_GRAPH_H_

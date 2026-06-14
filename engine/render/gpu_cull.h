#ifndef RECREATION_RENDER_GPU_CULL_H_
#define RECREATION_RENDER_GPU_CULL_H_

#include "core/math.h"
#include "render/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class Device;

// GPU-driven frustum culling for the opaque passes. Each frame the renderer
// fills a per-instance buffer (model + bounds) and a parallel indirect-command
// buffer (one VkDrawIndexedIndirectCommand per opaque submesh, instanceCount
// pre-set to 1). A compute pass tests every instance's world bounding sphere
// against the camera frustum and zeroes the instanceCount of culled draws, so
// the prepass and scene issue them as indirect draws and the gpu skips the
// off-screen ones. Buffers are host-visible: the cpu writes the static fields,
// the gpu writes instanceCount, no readback on the hot path.
class GpuCull {
 public:
  // Mirrors the Instance struct in cull.cs (std430).
  struct Instance {
    Mat4 model;
    f32 bounds[4];  // model-space sphere center.xyz, radius
    u32 first_cmd = 0;
    u32 cmd_count = 0;
    u32 cull_disabled = 0;
    u32 pad = 0;
  };
  // Matches VkDrawIndexedIndirectCommand.
  struct Command {
    u32 index_count = 0;
    u32 instance_count = 1;
    u32 first_index = 0;
    i32 vertex_offset = 0;
    u32 first_instance = 0;
  };

  static constexpr u32 kMaxCommands = 1u << 15;   // 32768 opaque submeshes
  static constexpr u32 kMaxInstances = 1u << 14;  // 16384 draws

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Begins filling the frame slot's buffers; returns mapped spans to append to.
  Instance* instances(u32 slot);
  Command* commands(u32 slot);
  VkBuffer command_buffer(u32 slot) const { return commands_[slot].buffer; }
  static constexpr u32 kCommandStride = sizeof(Command);

  // Records the cull dispatch + a barrier so the commands are ready for the
  // indirect draws. enabled=false keeps every instanceCount at 1 (no culling).
  void AddToGraph(RenderGraph& graph, const Mat4& view_proj, u32 instance_count, bool enabled,
                  u32 slot);

  // Visible draw count written by the previous frame's cull (one frame stale).
  u32 last_visible(u32 slot) const;

 private:
  static constexpr u32 kFramesInFlight = 2;

  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  GpuBuffer instances_[kFramesInFlight];
  GpuBuffer commands_[kFramesInFlight];
  GpuBuffer counts_[kFramesInFlight];
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_GPU_CULL_H_

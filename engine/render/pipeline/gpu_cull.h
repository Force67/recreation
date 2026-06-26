#ifndef RECREATION_RENDER_GPU_CULL_H_
#define RECREATION_RENDER_GPU_CULL_H_

#include "core/math.h"
#include "render/core/render_graph.h"
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

  // Sized for the densest streamed scenes (a full New Atlantis cell, or several
  // games' worldspaces loaded at once). The draw loops clamp to the count the
  // build actually wrote (cull_total_commands_), so exceeding these caps drops the
  // overflow geometry rather than reading past the indirect buffer (which renders
  // as garbage scanlines).
  static constexpr u32 kMaxCommands = 1u << 18;   // 262144 opaque submeshes
  static constexpr u32 kMaxInstances = 1u << 17;  // 131072 draws

  bool Initialize(Device& device, VkFormat color_format);
  void Destroy(Device& device);

  // Begins filling the frame slot's buffers; returns mapped spans to append to.
  Instance* instances(u32 slot);
  Command* commands(u32 slot);
  VkBuffer command_buffer(u32 slot) const { return commands_[slot].buffer; }
  static constexpr u32 kCommandStride = sizeof(Command);

  // (Re)creates the ping-pong depth snapshots and the hi-z reduce; call on init
  // and resize with the render resolution.
  void ResizeDepth(Device& device, u32 width, u32 height);
  // Reduces last frame's depth snapshot into a coarse farthest-depth hi-z that
  // the cull tests against this frame. Returns the hi-z handle (kInvalidResource
  // until ResizeDepth has run).
  ResourceHandle BuildHiZ(RenderGraph& graph, u32 slot);
  // Snapshots this frame's depth for next frame's occlusion test.
  void CopyDepth(RenderGraph& graph, ResourceHandle depth_export, u32 slot);

  // Records the cull dispatch + a barrier so the commands are ready for the
  // indirect draws. frustum=false keeps every instanceCount at 1 (no culling);
  // occlusion=false skips the hi-z test. hiz comes from BuildHiZ.
  void AddToGraph(RenderGraph& graph, const Mat4& view_proj, const Mat4& prev_view_proj,
                  const f32 proj_scale[2], const Vec3& eye, u32 instance_count, bool frustum,
                  bool occlusion, ResourceHandle hiz, u32 slot);

  // Debug view: wireframe boxes around each instance's world bounding sphere
  // (the cull / acceleration-structure bounds), overlaid on color.
  void AddBoundsPass(RenderGraph& graph, ResourceHandle color, const Mat4& view_proj,
                     u32 instance_count, u32 slot);

  // Visible draw count written by the previous frame's cull (one frame stale).
  u32 last_visible(u32 slot) const;

  // Dimensions of the coarse hi-z built by BuildHiZ, for shaders that sample it.
  u32 hiz_width() const { return hiz_w_; }
  u32 hiz_height() const { return hiz_h_; }

 private:
  static constexpr u32 kFramesInFlight = 2;
  bool CreateBoundsPipeline(Device& device, VkFormat color_format);

  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout bounds_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout bounds_layout_ = VK_NULL_HANDLE;
  VkPipeline bounds_pipeline_ = VK_NULL_HANDLE;
  GpuBuffer instances_[kFramesInFlight];
  GpuBuffer commands_[kFramesInFlight];
  GpuBuffer counts_[kFramesInFlight];

  // Occlusion culling: ping-pong full-res depth snapshots + a coarse hi-z reduce.
  static constexpr u32 kHizDownsample = 8;
  GpuImage prev_depth_[kFramesInFlight];
  VkImageLayout prev_depth_layout_[kFramesInFlight] = {VK_IMAGE_LAYOUT_UNDEFINED,
                                                       VK_IMAGE_LAYOUT_UNDEFINED};
  u32 depth_w_ = 0, depth_h_ = 0, hiz_w_ = 0, hiz_h_ = 0;
  VkDescriptorSetLayout hiz_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout hiz_layout_ = VK_NULL_HANDLE;
  VkPipeline hiz_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_GPU_CULL_H_

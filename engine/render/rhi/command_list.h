#ifndef RECREATION_RENDER_RHI_COMMAND_LIST_H_
#define RECREATION_RENDER_RHI_COMMAND_LIST_H_

#include <initializer_list>
#include <span>

#include "core/types.h"
#include "render/rhi/bindings.h"
#include "render/rhi/resources.h"
#include "render/rhi/types.h"

namespace rec::render {

// Image state transition. `before` must match the image's actual state
// (kUndefined discards). Mip range for mip-generation blit chains; count 0
// means "all remaining".
struct TextureBarrier {
  TextureHandle texture;
  ResourceState before = ResourceState::kUndefined;
  ResourceState after = ResourceState::kGeneral;
  u32 base_mip = 0;
  u32 mip_count = 0;
};

inline TextureBarrier Transition(const GpuImage& image, ResourceState before,
                                 ResourceState after) {
  return {.texture = image.handle, .before = before, .after = after};
}

struct ColorAttachment {
  TextureView view;
  LoadOp load = LoadOp::kClear;
  StoreOp store = StoreOp::kStore;
  f32 clear[4] = {0, 0, 0, 0};
};

struct DepthAttachment {
  TextureView view;
  LoadOp load = LoadOp::kClear;
  StoreOp store = StoreOp::kStore;
  f32 clear = 0.0f;  // reversed-z default
};

struct RenderingInfo {
  Extent2D extent;
  std::span<const ColorAttachment> colors;
  const DepthAttachment* depth = nullptr;  // null = no depth attachment
};

struct BufferTextureCopy {
  u64 buffer_offset = 0;
  u32 mip = 0;
  u32 array_layer = 0;
  Extent2D extent{};  // texel region at origin; {0,0} = whole mip
};

// BLAS build input: triangle geometry reached through device addresses, one
// entry per submesh range. Matches both VkAccelerationStructureGeometryKHR
// triangles and D3D12_RAYTRACING_GEOMETRY_DESC.
struct AccelTriangles {
  u64 vertex_address = 0;
  u32 vertex_stride = 0;
  u32 vertex_count = 0;
  Format vertex_format = Format::kRGB32Float;
  u64 index_address = 0;  // absolute address of this range's first index
  u32 index_count = 0;
  IndexType index_type = IndexType::kUint32;
  bool opaque = true;  // false = alpha-tested, ray queries run the any-hit loop
};

struct BlasBuildDesc {
  std::span<const AccelTriangles> geometries;
  bool fast_trace = true;  // PREFER_FAST_TRACE; false = PREFER_FAST_BUILD
};

// One TLAS instance. Layout is identical between
// VkAccelerationStructureInstanceKHR and D3D12_RAYTRACING_INSTANCE_DESC, so
// instance buffers upload as-is on both.
struct TlasInstance {
  f32 transform[3][4];               // row-major 3x4 world transform
  u32 custom_index : 24;             // instanceCustomIndex / InstanceID
  u32 mask : 8;
  u32 sbt_offset : 24;               // unused (ray queries only)
  u32 flags : 8;                     // TlasInstanceFlags
  u64 blas_address = 0;
};
static_assert(sizeof(TlasInstance) == 64, "tlas instance layout is api-shared");

enum TlasInstanceFlags : u8 {
  kTlasInstanceNone = 0,
  kTlasInstanceTriangleCullDisable = 1u << 0,
  kTlasInstanceForceOpaque = 1u << 2,
};

// Recording interface, implemented per backend. One instance is valid for one
// frame (or one immediate submit); pipelines, sets and push constants are
// stateful within it like the underlying APIs.
class CommandList {
 public:
  virtual ~CommandList() = default;

  // --- binding ---
  virtual void BindPipeline(PipelineHandle pipeline) = 0;
  // Persistent set (bindless registry, frame globals).
  virtual void BindSet(u32 set_index, BindingSetHandle set) = 0;
  // Transient bindings against the bound pipeline's layout: allocated from a
  // per-frame pool, written and bound in one call. This replaces the
  // allocate/update/bind descriptor dance.
  virtual void BindTransient(u32 set_index, std::span<const BindingItem> items) = 0;
  void BindTransient(u32 set_index, std::initializer_list<BindingItem> items) {
    BindTransient(set_index, std::span<const BindingItem>(items.begin(), items.size()));
  }
  // offset allows partial updates of a larger push block (e.g. a per-cascade
  // matrix at 0 with per-draw model matrices behind it). Maps to
  // vkCmdPushConstants offset / SetGraphicsRoot32BitConstants dest offset.
  virtual void PushConstants(const void* data, u32 size, u32 offset = 0) = 0;
  template <typename T>
  void Push(const T& constants, u32 offset = 0) {
    PushConstants(&constants, sizeof(T), offset);
  }

  // --- compute ---
  virtual void Dispatch(u32 x, u32 y, u32 z) = 0;
  // Ubiquitous fullscreen helper: one 8x8 thread group per tile.
  void Dispatch2D(Extent2D extent, u32 tile = 8) {
    Dispatch((extent.width + tile - 1) / tile, (extent.height + tile - 1) / tile, 1);
  }

  // --- raster ---
  // Begins dynamic rendering and sets viewport+scissor to the full extent.
  virtual void BeginRendering(const RenderingInfo& info) = 0;
  virtual void EndRendering() = 0;
  virtual void SetViewport(f32 x, f32 y, f32 width, f32 height) = 0;
  virtual void SetScissor(i32 x, i32 y, u32 width, u32 height) = 0;
  virtual void BindVertexBuffer(u32 binding, const GpuBuffer& buffer, u64 offset = 0) = 0;
  virtual void BindIndexBuffer(const GpuBuffer& buffer, u64 offset, IndexType type) = 0;
  virtual void Draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0,
                    u32 first_instance = 0) = 0;
  virtual void DrawIndexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0,
                           i32 vertex_offset = 0, u32 first_instance = 0) = 0;
  virtual void DrawIndexedIndirect(const GpuBuffer& args, u64 offset, u32 draw_count,
                                   u32 stride) = 0;
  virtual void DrawMeshTasks(u32 x, u32 y, u32 z) = 0;

  // --- synchronization ---
  virtual void TextureBarriers(std::span<const TextureBarrier> barriers) = 0;
  void Barrier(const TextureBarrier& barrier) {
    TextureBarriers(std::span<const TextureBarrier>(&barrier, 1));
  }
  // Global execution+memory barrier for buffer hazards (compute -> indirect
  // args, transfer -> shader read, AS build -> ray query, ...).
  virtual void MemoryBarrier(BarrierScope src, BarrierScope dst) = 0;

  // --- transfer ---
  virtual void CopyBufferToTexture(const GpuBuffer& src, const GpuImage& dst,
                                   std::span<const BufferTextureCopy> regions) = 0;
  virtual void CopyTextureToBuffer(const GpuImage& src, const GpuBuffer& dst,
                                   const BufferTextureCopy& region) = 0;
  virtual void CopyBuffer(const GpuBuffer& src, u64 src_offset, const GpuBuffer& dst,
                          u64 dst_offset, u64 size) = 0;
  // Linear-filtered blit mip -> mip (mip chain generation). Src mip must be in
  // kCopySrc, dst mip in kCopyDst.
  virtual void BlitMip(const GpuImage& image, u32 src_mip, Extent2D src_extent, u32 dst_mip,
                       Extent2D dst_extent) = 0;
  virtual void ClearColor(const GpuImage& image, const f32 color[4]) = 0;  // kCopyDst state
  virtual void ClearDepth(const GpuImage& image, f32 depth) = 0;           // kCopyDst state
  virtual void FillBuffer(const GpuBuffer& buffer, u64 offset, u64 size, u32 data) = 0;

  // --- acceleration structures (DeviceCaps::ray_query gated) ---
  virtual void BuildBlas(AccelStructHandle blas, const BlasBuildDesc& desc,
                         const GpuBuffer& scratch, u64 scratch_offset = 0) = 0;
  virtual void BuildTlas(AccelStructHandle tlas, const GpuBuffer& instances, u32 instance_count,
                         const GpuBuffer& scratch) = 0;

  // --- profiling ---
  virtual void ResetTimestamps(TimestampPoolHandle pool, u32 first, u32 count) = 0;
  virtual void WriteTimestamp(TimestampPoolHandle pool, u32 index, bool after_work) = 0;
  virtual void BeginDebugLabel(const char* name) = 0;
  virtual void EndDebugLabel() = 0;

  // Backend escape hatch (e.g. VkCommandBuffer for NRD/DLSS/FSR3/imgui interop
  // via rhi/vulkan_interop.h). Null on backends the caller does not know.
  virtual void* native_handle() = 0;

 protected:
  CommandList() = default;
  CommandList(const CommandList&) = delete;
  CommandList& operator=(const CommandList&) = delete;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_COMMAND_LIST_H_

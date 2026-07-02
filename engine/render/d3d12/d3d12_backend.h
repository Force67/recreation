#ifndef RECREATION_RENDER_D3D12_D3D12_BACKEND_H_
#define RECREATION_RENDER_D3D12_D3D12_BACKEND_H_

// Internal D3D12 backend types. Only d3d12/*.cc may include this; pass code
// sees rhi/ headers exclusively. On Windows this drives the real runtime; on
// Linux the D3D12 API is provided natively by vkd3d (libvkd3d +
// libvkd3d-utils), which is how the backend is validated on this repo's
// primary development machine. Differences that follow from the vkd3d 2.0
// baseline (legacy resource states instead of enhanced barriers, no DXGI,
// SM 6.5 DXIL) are called out inline and in RHI.md.

#include "render/d3d12/d3d12_headers.h"

#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"

namespace rec::render::d3d12 {

class D3D12Device;

// --- conversion tables (d3d12_convert.cc) ---

DXGI_FORMAT ToDxgiFormat(Format format);
// Format used for SRVs over the image (depth reads as R32_FLOAT).
DXGI_FORMAT ToDxgiSrvFormat(Format format);
D3D12_RESOURCE_STATES ToResourceStates(ResourceState state);
D3D12_COMPARISON_FUNC ToCompareFunc(CompareOp op);
D3D12_FILTER ToFilter(const SamplerDesc& desc);
D3D12_TEXTURE_ADDRESS_MODE ToAddressMode(AddressMode mode);

// --- backend records behind the opaque handles ---

struct BufferRecord {
  ID3D12Resource* resource = nullptr;
  u64 gpu_va = 0;
  u64 size = 0;
  // UPLOAD/READBACK heap resources live in a fixed state and are never
  // transitioned; DEFAULT/CUSTOM heap buffers decay to COMMON at every
  // ExecuteCommandLists boundary, so per-list state lives in the command list.
  bool fixed_state = false;
  bool allows_uav = false;
};

struct TextureViewRecord;

struct TextureRecord {
  ID3D12Resource* resource = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  Format rhi_format = Format::kUnknown;
  Extent2D extent{};
  u32 mip_levels = 1;
  u32 array_layers = 1;
  bool cube = false;
  bool owns_resource = true;  // false for swapchain wrappers? (offscreen owns)
  TextureUsageFlags usage = 0;
  // Legacy-barrier state tracking, one entry per subresource
  // (mip + layer * mip_levels), in recording order. The rhi passes explicit
  // before/after states but kUndefined means "whatever it was", which the
  // tracker resolves.
  base::Vector<D3D12_RESOURCE_STATES> sub_states;
  TextureViewRecord* default_view = nullptr;  // owned
};

// One rhi TextureView: a subresource window plus lazily created CPU
// descriptors (indices into the device's CPU descriptor pools, ~0u = not yet
// created).
struct TextureViewRecord {
  TextureRecord* texture = nullptr;
  u32 base_mip = 0;
  u32 mip_count = 0;  // 0 = all remaining
  bool force_array = false;
  u32 srv = ~0u;
  u32 uav = ~0u;
  u32 rtv = ~0u;
  u32 dsv = ~0u;
};

// A cached, immutable per-set descriptor layout: where every binding slot
// lands inside the set's view table and sampler table. Storage buffers occupy
// two view descriptors (SRV then UAV) because the rhi's kStorageBuffer covers
// both StructuredBuffer (t) and RWStructuredBuffer (u) declarations.
struct SetLayout {
  struct Slot {
    BindingSlot slot;
    u32 view_offset = ~0u;     // offset into the set's view table
    u32 sampler_offset = ~0u;  // offset into the set's sampler table
  };
  base::Vector<Slot> slots;
  u32 view_count = 0;
  u32 sampler_count = 0;
  u64 hash = 0;

  const Slot* Find(u32 binding) const {
    for (const Slot& s : slots) {
      if (s.slot.binding == binding) return &s;
    }
    return nullptr;
  }
};

struct BindingLayoutRecord {
  SetLayout* layout = nullptr;  // cached, device-owned
};

// Persistent set: a reserved region of the shader-visible heaps.
struct BindingSetRecord {
  SetLayout* layout = nullptr;
  u32 view_start = ~0u;     // index into the shader-visible view heap
  u32 sampler_start = ~0u;  // index into the shader-visible sampler heap
};

struct PipelineRecord {
  ID3D12PipelineState* pso = nullptr;
  ID3D12RootSignature* root = nullptr;  // cached, device-owned
  bool compute = true;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  u32 push_size = 0;
  bool push_root_constants = false;  // else: root CBV fed from the upload ring
  i32 push_param = -1;
  // Root SRV that emulates push-block buffer-device-address reads for DXIL
  // (bone palettes): bound with the u64 the backend finds at byte 128 of the
  // push block. See rec_bone_palette in mesh.vs.hlsl / RHI.md.
  i32 bda_param = -1;
  struct SetParams {
    SetLayout* layout = nullptr;
    i32 view_param = -1;
    i32 sampler_param = -1;
  };
  base::Vector<SetParams> sets;
  base::Vector<u32> vertex_strides;  // per vertex-buffer binding
};

struct AccelStructRecord {
  ID3D12Resource* resource = nullptr;
  u64 address = 0;
};

struct TimestampPoolRecord {
  ID3D12QueryHeap* heap = nullptr;
  ID3D12Resource* readback = nullptr;  // count * 8 bytes, persistently mapped
  void* mapped = nullptr;
  u32 count = 0;
};

// Handle <-> record casts (handles are record pointers).
inline BufferRecord* Rec(BufferHandle h) { return reinterpret_cast<BufferRecord*>(h.value); }
inline TextureRecord* Rec(TextureHandle h) { return reinterpret_cast<TextureRecord*>(h.value); }
inline BindingLayoutRecord* Rec(BindingLayoutHandle h) {
  return reinterpret_cast<BindingLayoutRecord*>(h.value);
}
inline BindingSetRecord* Rec(BindingSetHandle h) {
  return reinterpret_cast<BindingSetRecord*>(h.value);
}
inline PipelineRecord* Rec(PipelineHandle h) { return reinterpret_cast<PipelineRecord*>(h.value); }
inline AccelStructRecord* Rec(AccelStructHandle h) {
  return reinterpret_cast<AccelStructRecord*>(h.value);
}
inline TimestampPoolRecord* Rec(TimestampPoolHandle h) {
  return reinterpret_cast<TimestampPoolRecord*>(h.value);
}
inline TextureViewRecord* Rec(TextureView v) {
  return reinterpret_cast<TextureViewRecord*>(v.value);
}
template <typename Handle, typename Record>
inline Handle MakeHandle(Record* record) {
  return Handle{reinterpret_cast<u64>(record)};
}

// A fixed-capacity CPU (non-shader-visible) descriptor heap with a free list.
class CpuDescriptorPool {
 public:
  bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity);
  void Shutdown();
  u32 Alloc();
  void Free(u32 index);
  D3D12_CPU_DESCRIPTOR_HANDLE Cpu(u32 index) const {
    return {start_.ptr + static_cast<size_t>(index) * stride_};
  }

 private:
  ID3D12DescriptorHeap* heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE start_{};
  u32 stride_ = 0;
  u32 capacity_ = 0;
  u32 next_ = 0;
  base::Vector<u32> free_;
};

class D3D12CommandList final : public CommandList {
 public:
  D3D12CommandList(D3D12Device& device, ID3D12GraphicsCommandList* list, u32 ring_index)
      : device_(device), list_(list), ring_(ring_index) {}

  void BindPipeline(PipelineHandle pipeline) override;
  void BindSet(u32 set_index, BindingSetHandle set) override;
  void BindTransient(u32 set_index, std::span<const BindingItem> items) override;
  void PushConstants(const void* data, u32 size, u32 offset) override;
  void Dispatch(u32 x, u32 y, u32 z) override;
  void BeginRendering(const RenderingInfo& info) override;
  void EndRendering() override;
  void SetViewport(f32 x, f32 y, f32 width, f32 height) override;
  void SetScissor(i32 x, i32 y, u32 width, u32 height) override;
  void BindVertexBuffer(u32 binding, const GpuBuffer& buffer, u64 offset) override;
  void BindIndexBuffer(const GpuBuffer& buffer, u64 offset, IndexType type) override;
  void Draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) override;
  void DrawIndexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset,
                   u32 first_instance) override;
  void DrawIndexedIndirect(const GpuBuffer& args, u64 offset, u32 draw_count, u32 stride) override;
  void DrawMeshTasks(u32 x, u32 y, u32 z) override;
  void TextureBarriers(std::span<const TextureBarrier> barriers) override;
  void MemoryBarrier(BarrierScope src, BarrierScope dst) override;
  void CopyBufferToTexture(const GpuBuffer& src, const GpuImage& dst,
                           std::span<const BufferTextureCopy> regions) override;
  void CopyTextureToBuffer(const GpuImage& src, const GpuBuffer& dst,
                           const BufferTextureCopy& region) override;
  void CopyBuffer(const GpuBuffer& src, u64 src_offset, const GpuBuffer& dst, u64 dst_offset,
                  u64 size) override;
  void BlitMip(const GpuImage& image, u32 src_mip, Extent2D src_extent, u32 dst_mip,
               Extent2D dst_extent) override;
  void ClearColor(const GpuImage& image, const f32 color[4]) override;
  void ClearDepth(const GpuImage& image, f32 depth) override;
  void FillBuffer(const GpuBuffer& buffer, u64 offset, u64 size, u32 data) override;
  void BuildBlas(AccelStructHandle blas, const BlasBuildDesc& desc, const GpuBuffer& scratch,
                 u64 scratch_offset) override;
  void BuildTlas(AccelStructHandle tlas, const GpuBuffer& instances, u32 instance_count,
                 const GpuBuffer& scratch) override;
  void ResetTimestamps(TimestampPoolHandle pool, u32 first, u32 count) override;
  void WriteTimestamp(TimestampPoolHandle pool, u32 index, bool after_work) override;
  void BeginDebugLabel(const char* name) override;
  void EndDebugLabel() override;
  void* native_handle() override { return nullptr; }  // no vulkan interop here

  ID3D12GraphicsCommandList* list() const { return list_; }

  // Recording-session reset: called by the device after ID3D12CommandList
  // reset, before handing the list to the renderer.
  void OnBeginRecording();
  // Emits deferred work that must precede Close (timestamp resolves).
  void OnEndRecording();

  // Lazy legacy-state transition for buffers (per-list; buffers decay to
  // COMMON at every ExecuteCommandLists boundary).
  void RequireBufferState(BufferRecord* buffer, D3D12_RESOURCE_STATES state);
  // Explicit transition of a texture subresource range from its tracked state.
  void RequireTextureState(TextureRecord* texture, u32 base_mip, u32 mip_count,
                           D3D12_RESOURCE_STATES state);

 private:
  void FlushVertexBuffers();
  void SetPushRootCbv();

  D3D12Device& device_;
  ID3D12GraphicsCommandList* list_ = nullptr;
  u32 ring_ = 0;  // transient ring index this list allocates from
  const PipelineRecord* bound_ = nullptr;

  // Push-constant shadow block: PushConstants at an offset updates a slice
  // and re-uploads the pipeline's full block (matches vulkan push semantics
  // where earlier bytes persist).
  u8 push_shadow_[512] = {};

  struct PendingVertexBuffer {
    u64 va = 0;
    u64 size = 0;
    bool set = false;
  };
  PendingVertexBuffer pending_vb_[8];
  bool vb_dirty_ = false;

  base::UnorderedMap<u64, u32> buffer_states_;  // BufferRecord* -> state (this list)
  base::UnorderedMap<u64, u32> timestamp_max_;  // TimestampPoolRecord* -> max index + 1

  friend class D3D12Device;
};

// Swapchain. On Windows: a DXGI flip-model swapchain over the SDL window's
// HWND (compile-guarded, pending runtime validation on a Windows box). On
// Linux there is no DXGI through vkd3d, so the baseline is an offscreen ring
// of three presentable render targets: Acquire cycles them, "present" is a
// no-op, and the whole frame graph plus REC_UI_SHOT readback runs unchanged.
class D3D12Swapchain final : public Swapchain {
 public:
  static std::unique_ptr<D3D12Swapchain> Create(D3D12Device& device, u32 width, u32 height,
                                                bool vsync);
  ~D3D12Swapchain() override;

  AcquireResult Acquire(u32 slot, u32* out_image_index) override;
  Format format() const override { return format_; }
  Extent2D extent() const override { return extent_; }
  u32 image_count() const override { return static_cast<u32>(images_.size()); }
  const GpuImage& image(u32 index) const override { return images_[index]; }
  bool can_sample() const override { return true; }

  // Called by Device::SubmitFrame after the frame's ExecuteCommandLists.
  // DXGI Present on Windows; no-op offscreen.
  PresentResult Present();

 private:
  explicit D3D12Swapchain(D3D12Device& device) : device_(device) {}
  bool Init(u32 width, u32 height, bool vsync);

  D3D12Device& device_;
  Format format_ = Format::kBGRA8Unorm;
  Extent2D extent_{};
  base::Vector<GpuImage> images_;
  u64 frame_counter_ = 0;
#if defined(_WIN32)
  IDXGISwapChain3* dxgi_ = nullptr;
  bool vsync_ = true;
#endif
};

class D3D12Device final : public Device {
 public:
  static std::unique_ptr<Device> Create(const DeviceDesc& desc, Window& window);
  ~D3D12Device() override;

  void WaitIdle() override;
  bool RecreateSurface(Window& window) override { (void)window; return true; }
  void DestroySurface() override {}
  std::unique_ptr<Swapchain> CreateSwapchain(u32 width, u32 height, bool vsync) override;
  MemoryBudget memory_budget() const override;

  GpuBuffer CreateBuffer(u64 size, BufferUsageFlags usage, bool host_visible) override;
  GpuBuffer CreateBufferWithData(ByteSpan data, BufferUsageFlags usage) override;
  void DestroyBuffer(GpuBuffer& buffer) override;
  GpuImage CreateImage2D(Format format, Extent2D extent, TextureUsageFlags usage,
                         u32 mip_levels) override;
  GpuImage CreateImageCube(Format format, u32 size, TextureUsageFlags usage,
                           u32 mip_levels) override;
  void DestroyImage(GpuImage& image) override;
  TextureView CreateMipView(const GpuImage& image, u32 mip) override;
  TextureView CreateArrayView(const GpuImage& image) override;
  void DestroyView(TextureView view) override;
  SamplerHandle GetSampler(const SamplerDesc& desc) override;

  PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
  PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
  void DestroyPipeline(PipelineHandle pipeline) override;
  BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc& desc) override;
  void DestroyBindingLayout(BindingLayoutHandle layout) override;
  BindingSetHandle CreateBindingSet(BindingLayoutHandle layout, u32 variable_count) override;
  void DestroyBindingSet(BindingSetHandle set) override;
  void UpdateBindingSet(BindingSetHandle set, std::span<const BindingItem> items) override;

  AccelSizes GetBlasSizes(const BlasBuildDesc& desc) override;
  AccelSizes GetTlasSizes(u32 instance_count) override;
  AccelStructHandle CreateAccelStruct(AccelStructType type, u64 size) override;
  void DestroyAccelStruct(AccelStructHandle accel) override;
  u64 accel_address(AccelStructHandle accel) override;

  TimestampPoolHandle CreateTimestampPool(u32 count) override;
  void DestroyTimestampPool(TimestampPoolHandle pool) override;
  bool GetTimestamps(TimestampPoolHandle pool, u32 first, u32 count, u64* out) override;

  void ImmediateSubmit(const std::function<void(CommandList&)>& record) override;
  CommandList* BeginFrame(u32 slot) override;
  PresentResult SubmitFrame(CommandList* cmd, Swapchain& swapchain, u32 image_index) override;

  // --- backend internals ---

  ID3D12Device* device() const { return device_; }
  ID3D12CommandQueue* queue() const { return queue_; }
  void* native_window() const { return native_window_; }

  static constexpr u32 kRingCount = kMaxFramesInFlight + 1;  // frame slots + immediate
  static constexpr u32 kImmediateRing = kMaxFramesInFlight;

  // Shader-visible heap layout: [0, kPersistentViews) is the persistent-set
  // region (bindless registry, material sets, frame globals), the rest is
  // split into per-ring transient windows reset at BeginFrame.
  static constexpr u32 kPersistentViews = 65536;
  static constexpr u32 kTransientViews = 16384;
  static constexpr u32 kPersistentSamplers = 1536;  // shader-visible cap is 2048
  static constexpr u32 kPushRingBytes = 4u << 20;

  u32 view_stride() const { return view_stride_; }
  u32 sampler_stride() const { return sampler_stride_; }
  D3D12_CPU_DESCRIPTOR_HANDLE ViewCpu(u32 index) const {
    return {view_heap_cpu_.ptr + static_cast<size_t>(index) * view_stride_};
  }
  D3D12_GPU_DESCRIPTOR_HANDLE ViewGpu(u32 index) const {
    return {view_heap_gpu_.ptr + static_cast<u64>(index) * view_stride_};
  }
  D3D12_CPU_DESCRIPTOR_HANDLE SamplerCpuVisible(u32 index) const {
    return {sampler_heap_cpu_.ptr + static_cast<size_t>(index) * sampler_stride_};
  }
  D3D12_GPU_DESCRIPTOR_HANDLE SamplerGpu(u32 index) const {
    return {sampler_heap_gpu_.ptr + static_cast<u64>(index) * sampler_stride_};
  }

  // Transient view-table allocation from ring `ring`; returns heap index or
  // ~0u when the ring window is exhausted.
  u32 AllocTransientViews(u32 ring, u32 count);
  // Push-constant upload ring slice (256-aligned); returns GPU VA and a CPU
  // write pointer.
  u64 AllocPushSlice(u32 ring, u32 size, void** cpu);

  // Sampler tables are cached forever, keyed by their handle content.
  u32 GetSamplerTable(const u64* sampler_cpu_indices, u32 count);

  // Descriptor writes shared by BindTransient and UpdateBindingSet.
  void WriteViewDescriptor(const SetLayout::Slot& slot, const BindingItem& item,
                           u32 table_start_index);
  void WriteNullViewDescriptors(const SetLayout& layout, u32 table_start_index);
  void WriteSamplerDescriptor(u32 heap_index, SamplerHandle sampler);

  u32 EnsureSrv(TextureViewRecord* view);
  u32 EnsureUav(TextureViewRecord* view);
  u32 EnsureRtv(TextureViewRecord* view);
  u32 EnsureDsv(TextureViewRecord* view);
  CpuDescriptorPool& srv_pool() { return srv_pool_; }
  CpuDescriptorPool& rtv_pool() { return rtv_pool_; }
  CpuDescriptorPool& dsv_pool() { return dsv_pool_; }

  SetLayout* GetOrCreateSetLayout(const BindingLayoutDesc& desc);
  ID3D12RootSignature* GetOrCreateRootSignature(std::span<SetLayout* const> sets, u32 push_size,
                                                bool push_root_constants,
                                                PipelineRecord* out_params);
  ID3D12CommandSignature* GetDrawIndexedSignature(u32 stride);

  // Internal texture-to-texture filtered blit pipeline (BlitMip); one PSO per
  // destination format.
  PipelineHandle GetBlitPipeline(Format format);
  SamplerHandle blit_sampler();

  // Deferred destruction of internal staging resources created during
  // recording (drained after the ring's fence has passed).
  void DeferRelease(u32 ring, ID3D12Resource* resource);

  TextureRecord* MakeTextureRecord(ID3D12Resource* resource, Format format, Extent2D extent,
                                   u32 mips, u32 layers, bool cube, TextureUsageFlags usage);
  GpuImage WrapTexture(TextureRecord* record);

  u64 frame_epoch() const { return frame_epoch_; }

 private:
  D3D12Device() = default;
  bool InitResources();
  void ShutdownResources();
  void SignalAndWait();
  D3D12CommandList* BeginRing(u32 ring);
  void CloseAndExecute(u32 ring);

  ID3D12Device* device_ = nullptr;
  ID3D12Device5* device5_ = nullptr;  // raytracing (may be null)
  ID3D12Device2* device2_ = nullptr;  // pipeline state streams (may be null)
  ID3D12CommandQueue* queue_ = nullptr;
  void* native_window_ = nullptr;  // SDL_Window*, for the DXGI HWND on windows

  ID3D12DescriptorHeap* view_heap_ = nullptr;
  ID3D12DescriptorHeap* sampler_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE view_heap_cpu_{};
  D3D12_GPU_DESCRIPTOR_HANDLE view_heap_gpu_{};
  D3D12_CPU_DESCRIPTOR_HANDLE sampler_heap_cpu_{};
  D3D12_GPU_DESCRIPTOR_HANDLE sampler_heap_gpu_{};
  u32 view_stride_ = 0;
  u32 sampler_stride_ = 0;

  CpuDescriptorPool srv_pool_;      // CPU-side SRV/UAV/CBV descriptors
  CpuDescriptorPool rtv_pool_;
  CpuDescriptorPool dsv_pool_;
  CpuDescriptorPool sampler_pool_;  // CPU-side sampler prototypes

  // Persistent region allocator (first-fit free ranges).
  struct FreeRange {
    u32 start;
    u32 count;
  };
  base::Vector<FreeRange> persistent_free_;
  u32 sampler_persistent_next_ = 0;

  struct Ring {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    u64 fence_value = 0;
    u32 view_base = 0;    // transient window start in the view heap
    u32 view_cursor = 0;
    ID3D12Resource* push_ring = nullptr;
    u8* push_mapped = nullptr;
    u64 push_cursor = 0;
    std::unique_ptr<D3D12CommandList> wrapper;
    base::Vector<ID3D12Resource*> deferred;
    bool warned_views = false;
  };
  Ring rings_[kRingCount];
  u64 frame_epoch_ = 0;

  base::UnorderedMap<u64, SetLayout*> set_layout_cache_;
  base::UnorderedMap<u64, ID3D12RootSignature*> root_signature_cache_;
  base::UnorderedMap<u64, u64> sampler_cache_;        // desc hash -> cpu pool index + 1
  base::UnorderedMap<u64, u32> sampler_table_cache_;  // content hash -> heap start
  base::UnorderedMap<u64, ID3D12CommandSignature*> command_signature_cache_;
  base::UnorderedMap<u64, PipelineHandle> blit_pipeline_cache_;
  SamplerHandle blit_sampler_{};

  friend class D3D12CommandList;
  friend class D3D12Swapchain;
};

}  // namespace rec::render::d3d12

#endif  // RECREATION_RENDER_D3D12_D3D12_BACKEND_H_

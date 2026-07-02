#ifndef RECREATION_RENDER_RHI_DEVICE_H_
#define RECREATION_RENDER_RHI_DEVICE_H_

#include <functional>
#include <memory>
#include <string>

#include "core/types.h"
#include "core/window.h"
#include "render/rhi/bindings.h"
#include "render/rhi/command_list.h"
#include "render/rhi/pipeline.h"
#include "render/rhi/resources.h"
#include "render/rhi/types.h"

namespace rec::render {

class Swapchain;

enum class Backend : u8 {
  kAuto = 0,  // first available: platform native, then vulkan, then null
  kVulkan,
  kD3D12,
  kNull,  // headless stub: creates nothing, records nothing; keeps the
          // renderer's control flow testable without a GPU or a loader
};

const char* BackendName(Backend backend);

struct DeviceDesc {
  Backend backend = Backend::kAuto;
  bool enable_validation = false;
  bool request_raytracing = true;
};

// What the picked GPU actually supports. Optional features are queried,
// never assumed, so the same binary runs on a desktop GPU and an android
// phone. Per-backend baselines: Vulkan 1.3 with dynamic rendering,
// synchronization2, buffer device address, descriptor indexing and timeline
// semaphores; D3D12 feature level 12_1 with SM 6.6 and enhanced barriers.
struct DeviceCaps {
  Backend backend = Backend::kNull;
  std::string adapter_name;
  u32 api_version = 0;
  bool raytracing = false;  // acceleration structures + ray tracing pipeline
  bool ray_query = false;
  bool mesh_shaders = false;
  bool fragment_shading_rate = false;
  bool fill_mode_non_solid = false;  // wireframe debug views
  f32 max_anisotropy = 1.0f;         // 1 = anisotropic filtering unavailable
  f32 timestamp_period = 0.0f;       // ns per timestamp tick, 0 = no gpu timing
  bool debug_utils = false;          // debug markers/labels available
  bool integrated = false;           // integrated/handheld gpu (shared memory)
  u64 device_local_bytes = 0;        // summed device-local heap size, vram proxy
  u32 accel_scratch_alignment = 256;  // min scratch offset alignment for AS builds
};

enum class AccelStructType : u8 { kBlas, kTlas };

struct AccelSizes {
  u64 accel_bytes = 0;
  u64 scratch_bytes = 0;
};

enum class PresentResult : u8 {
  kOk,
  kOutOfDate,  // recreate the swapchain (includes real-resize suboptimal)
  kFailed,
};

// Owns the GPU: adapter selection, resource + pipeline creation, per-frame
// command recording and submission. Create() picks the backend from the desc;
// a null-backend device (is_stub() true) comes back when no loader, no
// capable GPU or no presentable window is available, and every operation on
// it is a safe no-op.
class Device {
 public:
  static constexpr u32 kMaxFramesInFlight = 2;

  static std::unique_ptr<Device> Create(const DeviceDesc& desc, Window& window);
  virtual ~Device() = default;

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const DeviceCaps& caps() const { return caps_; }
  bool is_stub() const { return caps_.backend == Backend::kNull; }

  virtual void WaitIdle() = 0;

  // Rebinds the presentation surface to a (possibly new) window after it was
  // lost, for platforms that destroy and recreate the surface across the app
  // lifecycle (Android background/foreground). Adapter and queues are
  // unchanged. Returns false if the new surface could not be created.
  virtual bool RecreateSurface(Window& window) = 0;

  // Destroys the presentation surface (its swapchain must already be gone).
  virtual void DestroySurface() = 0;

  // `hdr` requests an HDR-capable surface format (HDR10 PQ preferred, scRGB
  // fallback); silently falls back to SDR when the surface has neither.
  virtual std::unique_ptr<Swapchain> CreateSwapchain(u32 width, u32 height, bool vsync,
                                                     bool hdr = false) = 0;

  // Live gpu memory usage, summed over the device-local heaps, for the debug
  // overlay. budget is the driver's estimate of what the app may use.
  struct MemoryBudget {
    u64 used_bytes = 0;
    u64 budget_bytes = 0;
    u64 allocated_bytes = 0;
    u32 allocation_count = 0;
  };
  virtual MemoryBudget memory_budget() const = 0;

  // --- resources ---
  virtual GpuBuffer CreateBuffer(u64 size, BufferUsageFlags usage, bool host_visible = false) = 0;
  virtual GpuBuffer CreateBufferWithData(ByteSpan data, BufferUsageFlags usage) = 0;
  virtual void DestroyBuffer(GpuBuffer& buffer) = 0;

  virtual GpuImage CreateImage2D(Format format, Extent2D extent, TextureUsageFlags usage,
                                 u32 mip_levels = 1) = 0;
  virtual GpuImage CreateImageCube(Format format, u32 size, TextureUsageFlags usage,
                                   u32 mip_levels = 1) = 0;
  virtual void DestroyImage(GpuImage& image) = 0;
  // Extra single-mip view for mip-chained images (bloom pyramid).
  virtual TextureView CreateMipView(const GpuImage& image, u32 mip) = 0;
  // 2D-array view over the whole image, for shaders that declare
  // Texture2DArray over a (possibly single-layer) atlas.
  virtual TextureView CreateArrayView(const GpuImage& image) = 0;
  virtual void DestroyView(TextureView view) = 0;

  // Cached; valid for the device's lifetime, never destroyed by callers.
  virtual SamplerHandle GetSampler(const SamplerDesc& desc) = 0;

  // --- pipelines & bindings ---
  virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
  virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
  virtual void DestroyPipeline(PipelineHandle pipeline) = 0;

  // Explicit layouts, for sets shared across pipelines (bindless registry,
  // frame globals). Pass-local sets skip this and declare slots inline in the
  // pipeline desc.
  virtual BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc& desc) = 0;
  virtual void DestroyBindingLayout(BindingLayoutHandle layout) = 0;
  // Persistent set (lives across frames, updated incrementally). Transient
  // per-record sets go through CommandList::BindTransient instead.
  virtual BindingSetHandle CreateBindingSet(BindingLayoutHandle layout,
                                            u32 variable_count = 0) = 0;
  virtual void DestroyBindingSet(BindingSetHandle set) = 0;
  virtual void UpdateBindingSet(BindingSetHandle set, std::span<const BindingItem> items) = 0;
  void UpdateBindingSet(BindingSetHandle set, std::initializer_list<BindingItem> items) {
    UpdateBindingSet(set, std::span<const BindingItem>(items.begin(), items.size()));
  }

  // --- acceleration structures (caps().ray_query gated) ---
  virtual AccelSizes GetBlasSizes(const BlasBuildDesc& desc) = 0;
  virtual AccelSizes GetTlasSizes(u32 instance_count) = 0;
  virtual AccelStructHandle CreateAccelStruct(AccelStructType type, u64 size) = 0;
  virtual void DestroyAccelStruct(AccelStructHandle accel) = 0;
  virtual u64 accel_address(AccelStructHandle accel) = 0;

  // --- profiling ---
  virtual TimestampPoolHandle CreateTimestampPool(u32 count) = 0;
  virtual void DestroyTimestampPool(TimestampPoolHandle pool) = 0;
  // Copies available results (ticks) for [first, first+count); returns false
  // while the range is still in flight.
  virtual bool GetTimestamps(TimestampPoolHandle pool, u32 first, u32 count, u64* out) = 0;

  // --- recording & submission ---
  // Records into a transient command list and blocks until execution
  // finished. For uploads and one-off transitions, not the frame path.
  virtual void ImmediateSubmit(const std::function<void(CommandList&)>& record) = 0;

  // Frame ring: waits for `slot`'s previous submission, resets its command
  // allocator and transient binding pool, begins recording. Slots cycle
  // 0..kMaxFramesInFlight-1.
  virtual CommandList* BeginFrame(u32 slot) = 0;
  // Ends recording, submits waiting on the slot's swapchain acquire, presents
  // image_index. Backends fold "suboptimal but same extent" (Android rotation)
  // into kOk; kOutOfDate means the caller must recreate the swapchain.
  virtual PresentResult SubmitFrame(CommandList* cmd, Swapchain& swapchain, u32 image_index) = 0;

 protected:
  Device() = default;

  DeviceCaps caps_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_DEVICE_H_

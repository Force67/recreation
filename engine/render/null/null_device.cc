// Null backend: a device that creates nothing and records nothing. Keeps the
// engine's control flow alive (headless servers, machines without a GPU or
// loader) and proves the rhi interface implementable without any graphics API.

#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"

namespace rec::render::null {
namespace {

class NullCommandList final : public CommandList {
 public:
  void BindPipeline(PipelineHandle) override {}
  void BindSet(u32, BindingSetHandle) override {}
  void BindTransient(u32, std::span<const BindingItem>) override {}
  void PushConstants(const void*, u32, u32) override {}
  void Dispatch(u32, u32, u32) override {}
  void BeginRendering(const RenderingInfo&) override {}
  void EndRendering() override {}
  void SetViewport(f32, f32, f32, f32) override {}
  void SetScissor(i32, i32, u32, u32) override {}
  void BindVertexBuffer(u32, const GpuBuffer&, u64) override {}
  void BindIndexBuffer(const GpuBuffer&, u64, IndexType) override {}
  void Draw(u32, u32, u32, u32) override {}
  void DrawIndexed(u32, u32, u32, i32, u32) override {}
  void DrawIndexedIndirect(const GpuBuffer&, u64, u32, u32) override {}
  void DrawMeshTasks(u32, u32, u32) override {}
  void TextureBarriers(std::span<const TextureBarrier>) override {}
  void MemoryBarrier(BarrierScope, BarrierScope) override {}
  void CopyBufferToTexture(const GpuBuffer&, const GpuImage&,
                           std::span<const BufferTextureCopy>) override {}
  void CopyTextureToBuffer(const GpuImage&, const GpuBuffer&,
                           const BufferTextureCopy&) override {}
  void CopyBuffer(const GpuBuffer&, u64, const GpuBuffer&, u64, u64) override {}
  void BlitMip(const GpuImage&, u32, Extent2D, u32, Extent2D) override {}
  void ClearColor(const GpuImage&, const f32[4]) override {}
  void ClearDepth(const GpuImage&, f32) override {}
  void FillBuffer(const GpuBuffer&, u64, u64, u32) override {}
  void BuildBlas(AccelStructHandle, const BlasBuildDesc&, const GpuBuffer&, u64) override {}
  void BuildTlas(AccelStructHandle, const GpuBuffer&, u32, const GpuBuffer&) override {}
  void ResetTimestamps(TimestampPoolHandle, u32, u32) override {}
  void WriteTimestamp(TimestampPoolHandle, u32, bool) override {}
  void BeginDebugLabel(const char*) override {}
  void EndDebugLabel() override {}
  void* native_handle() override { return nullptr; }
};

class NullDevice final : public Device {
 public:
  NullDevice() { caps_.backend = Backend::kNull; }

  void WaitIdle() override {}
  bool RecreateSurface(Window&) override { return false; }
  void DestroySurface() override {}
  std::unique_ptr<Swapchain> CreateSwapchain(u32, u32, bool, bool) override { return nullptr; }
  MemoryBudget memory_budget() const override { return {}; }

  GpuBuffer CreateBuffer(u64, BufferUsageFlags, bool) override { return {}; }
  GpuBuffer CreateBufferWithData(ByteSpan, BufferUsageFlags) override { return {}; }
  void DestroyBuffer(GpuBuffer& buffer) override { buffer = {}; }
  GpuImage CreateImage2D(Format, Extent2D, TextureUsageFlags, u32) override { return {}; }
  GpuImage CreateImageCube(Format, u32, TextureUsageFlags, u32) override { return {}; }
  void DestroyImage(GpuImage& image) override { image = {}; }
  TextureView CreateMipView(const GpuImage&, u32) override { return {}; }
  TextureView CreateArrayView(const GpuImage&) override { return {}; }
  void DestroyView(TextureView) override {}
  SamplerHandle GetSampler(const SamplerDesc&) override { return {}; }

  PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return {}; }
  PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return {}; }
  void DestroyPipeline(PipelineHandle) override {}
  BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc&) override { return {}; }
  void DestroyBindingLayout(BindingLayoutHandle) override {}
  BindingSetHandle CreateBindingSet(BindingLayoutHandle, u32) override { return {}; }
  void DestroyBindingSet(BindingSetHandle) override {}
  void UpdateBindingSet(BindingSetHandle, std::span<const BindingItem>) override {}

  AccelSizes GetBlasSizes(const BlasBuildDesc&) override { return {}; }
  AccelSizes GetTlasSizes(u32) override { return {}; }
  AccelStructHandle CreateAccelStruct(AccelStructType, u64) override { return {}; }
  void DestroyAccelStruct(AccelStructHandle) override {}
  u64 accel_address(AccelStructHandle) override { return 0; }

  TimestampPoolHandle CreateTimestampPool(u32) override { return {}; }
  void DestroyTimestampPool(TimestampPoolHandle) override {}
  bool GetTimestamps(TimestampPoolHandle, u32, u32, u64*) override { return false; }

  void ImmediateSubmit(const std::function<void(CommandList&)>& record) override {
    NullCommandList cmd;
    record(cmd);
  }
  CommandList* BeginFrame(u32) override { return &cmd_; }
  PresentResult SubmitFrame(CommandList*, Swapchain&, u32) override {
    return PresentResult::kFailed;
  }

 private:
  NullCommandList cmd_;
};

}  // namespace

std::unique_ptr<Device> CreateNullDevice() { return std::make_unique<NullDevice>(); }

}  // namespace rec::render::null

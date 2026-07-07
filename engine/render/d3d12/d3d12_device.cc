// D3D12 backend device. On Windows the device comes from d3d12.dll via DXGI
// adapter enumeration; on Linux it comes from vkd3d's native
// D3D12CreateDevice (libvkd3d-utils), which lowers everything onto Vulkan.
// The implementation targets the vkd3d 2.0 feature set: legacy resource
// states (no enhanced barriers), no DXGI (offscreen swapchain), SM 6.5 DXIL.
// Committed resources only for now; suballocation (D3D12MA) is a TODO.

#define INITGUID
#include "render/d3d12/d3d12_backend.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "core/log.h"

// Internal blit shaders (BlitMip lowers to a fullscreen draw; D3D12 has no
// filtered copy). Embedded by the build like every pass shader.
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/blit_ps_hlsl.h"

namespace rec::render::d3d12 {
namespace {

u64 HashBytes(const void* data, size_t size, u64 seed = 1469598103934665603ull) {
  const u8* bytes = static_cast<const u8*>(data);
  u64 hash = seed;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 HashLayoutDesc(const BindingLayoutDesc& desc) {
  // Stages are irrelevant on d3d12 (visibility ALL); hash only the slots so
  // vulkan-stage-only differences share one layout.
  u64 hash = 1469598103934665603ull;
  for (const BindingSlot& slot : desc.slots) hash = HashBytes(&slot, sizeof(slot), hash);
  return hash;
}

constexpr u32 kInvalidIndex = ~0u;

// Push constants above this spill into a per-frame upload ring behind a root
// CBV; below they ride as root constants (b999, space0 in the HLSL).
constexpr u32 kMaxRootConstantBytes = 64;

struct BlendConfig {
  bool enable;
  D3D12_BLEND src_color, dst_color, src_alpha, dst_alpha;
};

BlendConfig BlendConfigOf(BlendMode mode) {
  switch (mode) {
    case BlendMode::kOpaque:
      return {false, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_ZERO};
    case BlendMode::kAlpha:
      return {true, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_ONE,
              D3D12_BLEND_INV_SRC_ALPHA};
    case BlendMode::kPremultiplied:
      return {true, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_ONE,
              D3D12_BLEND_INV_SRC_ALPHA};
    case BlendMode::kAdditive:
      return {true, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE};
    case BlendMode::kMultiply:
      return {true, D3D12_BLEND_ZERO, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_ZERO,
              D3D12_BLEND_SRC_ALPHA};
    case BlendMode::kWboitAccum:
      return {true, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE};
    case BlendMode::kWboitReveal:
      return {true, D3D12_BLEND_ZERO, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_ZERO,
              D3D12_BLEND_INV_SRC_ALPHA};
  }
  return {false, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_ZERO};
}

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

// --- DXIL container input-signature parsing ---
//
// D3D12 input layouts bind vertex attributes by semantic name, which the rhi
// deliberately does not carry (Vulkan matches by location). The ISG1 part of
// the DXIL container has the semantics in element order, and dxc assigns both
// SPIR-V locations and signature order from the declaration order, so the
// pipeline desc's attributes (sorted by location) pair 1:1 with the
// signature's non-system-value elements (sorted by register).
struct SignatureElement {
  std::string name;
  u32 semantic_index = 0;
  u32 reg = 0;
};

bool ParseInputSignature(const void* dxil, size_t size, base::Vector<SignatureElement>* out) {
  const u8* bytes = static_cast<const u8*>(dxil);
  auto rd = [&](size_t offset) -> u32 {
    u32 v;
    std::memcpy(&v, bytes + offset, 4);
    return v;
  };
  if (size < 32 || std::memcmp(bytes, "DXBC", 4) != 0) return false;
  u32 part_count = rd(28);
  for (u32 p = 0; p < part_count; ++p) {
    u32 part_offset = rd(32 + p * 4);
    if (part_offset + 8 > size) return false;
    if (std::memcmp(bytes + part_offset, "ISG1", 4) != 0) continue;
    const u8* data = bytes + part_offset + 8;
    u32 count = 0, header = 0;
    std::memcpy(&count, data, 4);
    std::memcpy(&header, data + 4, 4);
    const u8* e = data + header;  // header dwords include count+size
    for (u32 i = 0; i < count; ++i, e += 32) {
      // ISG1 element: stream, name_offset, semantic_index, sysval,
      // component_type, register, mask/rw_mask, min_precision (8 dwords).
      u32 name_offset, semantic_index, sysval, reg;
      std::memcpy(&name_offset, e + 4, 4);
      std::memcpy(&semantic_index, e + 8, 4);
      std::memcpy(&sysval, e + 12, 4);
      std::memcpy(&reg, e + 20, 4);
      if (sysval != 0) continue;  // SV_VertexID etc: not vertex-buffer fed
      SignatureElement element;
      element.name = reinterpret_cast<const char*>(data + name_offset);
      element.semantic_index = semantic_index;
      element.reg = reg;
      out->push_back(std::move(element));
    }
    return true;
  }
  return false;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(PrimitiveTopology topology) {
  switch (topology) {
    case PrimitiveTopology::kTriangleList:
    case PrimitiveTopology::kTriangleStrip:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    case PrimitiveTopology::kLineList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case PrimitiveTopology::kPointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  }
  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

D3D12_PRIMITIVE_TOPOLOGY DrawTopology(PrimitiveTopology topology) {
  switch (topology) {
    case PrimitiveTopology::kTriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::kTriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::kLineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::kPointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  }
  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

D3D12_BLEND_DESC BuildBlendDesc(const base::Vector<Format>& color_formats,
                                const base::Vector<BlendMode>& blend) {
  D3D12_BLEND_DESC desc = {};
  for (size_t i = 0; i < color_formats.size() && i < 8; ++i) {
    BlendMode mode = i < blend.size() ? blend[i] : BlendMode::kOpaque;
    BlendConfig config = BlendConfigOf(mode);
    D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[i];
    rt.BlendEnable = config.enable ? TRUE : FALSE;
    rt.SrcBlend = config.src_color;
    rt.DestBlend = config.dst_color;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = config.src_alpha;
    rt.DestBlendAlpha = config.dst_alpha;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  return desc;
}

D3D12_RASTERIZER_DESC BuildRasterDesc(const RasterState& raster, const DepthState& depth) {
  D3D12_RASTERIZER_DESC desc = {};
  desc.FillMode =
      raster.polygon == PolygonMode::kLine ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  desc.CullMode = raster.cull == CullMode::kNone   ? D3D12_CULL_MODE_NONE
                  : raster.cull == CullMode::kBack ? D3D12_CULL_MODE_BACK
                                                   : D3D12_CULL_MODE_FRONT;
  // The backend renders with an inverted viewport (negative height) so the
  // shared Vulkan-tuned HLSL/matrices produce the same image on both APIs;
  // screen-space winding therefore matches the vulkan backend directly.
  desc.FrontCounterClockwise = raster.front == FrontFace::kCounterClockwise ? TRUE : FALSE;
  desc.DepthBias = static_cast<INT>(depth.bias_constant);
  desc.SlopeScaledDepthBias = depth.bias_slope;
  desc.DepthClipEnable = TRUE;
  return desc;
}

D3D12_DEPTH_STENCIL_DESC BuildDepthDesc(const DepthState& depth) {
  D3D12_DEPTH_STENCIL_DESC desc = {};
  desc.DepthEnable = depth.test ? TRUE : FALSE;
  desc.DepthWriteMask = depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthFunc = ToCompareFunc(depth.compare);
  return desc;
}

// Pipeline-state-stream subobject, CD3DX12-layout compatible.
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Value>
struct alignas(void*) StreamSubobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Value value{};
};

}  // namespace

// --- CpuDescriptorPool ---

bool CpuDescriptorPool::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity) {
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = type;
  desc.NumDescriptors = capacity;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_ID3D12DescriptorHeap,
                                          reinterpret_cast<void**>(&heap_)))) {
    return false;
  }
  start_ = heap_->GetCPUDescriptorHandleForHeapStart();
  stride_ = device->GetDescriptorHandleIncrementSize(type);
  capacity_ = capacity;
  return true;
}

void CpuDescriptorPool::Shutdown() { SafeRelease(heap_); }

u32 CpuDescriptorPool::Alloc() {
  if (!free_.empty()) {
    u32 index = free_.back();
    free_.pop_back();
    return index;
  }
  if (next_ >= capacity_) {
    REC_ERROR("d3d12: cpu descriptor pool exhausted ({} descriptors)", capacity_);
    return 0;
  }
  return next_++;
}

void CpuDescriptorPool::Free(u32 index) { free_.push_back(index); }

// --- device creation ---

std::unique_ptr<Device> D3D12Device::Create(const DeviceDesc& desc, Window& window) {
  auto device = std::unique_ptr<D3D12Device>(new D3D12Device());
  // Unused on linux (offscreen swapchain); the DXGI swapchain pulls the HWND
  // out of the SDL window on windows.
  device->native_window_ = window.native_handles().window;

#if defined(_WIN32)
  if (desc.enable_validation) {
    ID3D12Debug* debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_ID3D12Debug, reinterpret_cast<void**>(&debug)))) {
      debug->EnableDebugLayer();
      debug->Release();
    }
  }
#endif

  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12Device,
                               reinterpret_cast<void**>(&device->device_)))) {
    // Feature level 12_0 keeps binding tier 2+; retry 11_0 for vkd3d setups
    // that report lower levels on otherwise capable drivers.
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                 reinterpret_cast<void**>(&device->device_)))) {
      REC_WARN("d3d12: no capable device (D3D12CreateDevice failed), backend unavailable");
      return nullptr;
    }
  }
  ID3D12Device* dev = device->device_;

  dev->QueryInterface(IID_ID3D12Device5, reinterpret_cast<void**>(&device->device5_));
  dev->QueryInterface(IID_ID3D12Device2, reinterpret_cast<void**>(&device->device2_));

  device->caps_.backend = Backend::kD3D12;
#if defined(_WIN32)
  device->caps_.adapter_name = "d3d12 adapter";
#else
  device->caps_.adapter_name = "vkd3d (D3D12-on-Vulkan)";
#endif
  device->caps_.api_version = 12;
  device->caps_.fill_mode_non_solid = true;
  device->caps_.max_anisotropy = 16.0f;
  device->caps_.debug_utils = false;
  device->caps_.accel_scratch_alignment = 256;

  D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch)))) {
    device->caps_.integrated = arch.UMA != FALSE;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                         sizeof(options5)))) {
    bool tier11 = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    device->caps_.raytracing = desc.request_raytracing && tier11 && device->device5_;
    device->caps_.ray_query = device->caps_.raytracing;  // ray queries are tier 1.1
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                         sizeof(options7)))) {
    device->caps_.mesh_shaders =
        options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED && device->device2_;
  }
  D3D12_FEATURE_DATA_SHADER_MODEL shader_model = {D3D_SHADER_MODEL_6_6};
  dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model));

  if (!device->InitResources()) return nullptr;

  u64 frequency = 0;
  if (SUCCEEDED(device->queue_->GetTimestampFrequency(&frequency)) && frequency > 0) {
    device->caps_.timestamp_period = 1e9f / static_cast<f32>(frequency);
  }

  REC_INFO("gpu: {} (d3d12, sm {:x}, rt={} rayquery={} mesh={} uma={})",
           device->caps_.adapter_name, static_cast<u32>(shader_model.HighestShaderModel),
           device->caps_.raytracing, device->caps_.ray_query, device->caps_.mesh_shaders,
           device->caps_.integrated);
  return device;
}

std::unique_ptr<Device> CreateD3D12Device(const DeviceDesc& desc, Window& window) {
  return D3D12Device::Create(desc, window);
}

bool D3D12Device::InitResources() {
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  if (FAILED(device_->CreateCommandQueue(&queue_desc, IID_ID3D12CommandQueue,
                                         reinterpret_cast<void**>(&queue_)))) {
    REC_ERROR("d3d12: command queue creation failed");
    return false;
  }

  view_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

  D3D12_DESCRIPTOR_HEAP_DESC view_desc = {};
  view_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  view_desc.NumDescriptors = kPersistentViews + kRingCount * kTransientViews;
  view_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&view_desc, IID_ID3D12DescriptorHeap,
                                           reinterpret_cast<void**>(&view_heap_)))) {
    REC_ERROR("d3d12: shader-visible view heap creation failed");
    return false;
  }
  view_heap_cpu_ = view_heap_->GetCPUDescriptorHandleForHeapStart();
  view_heap_gpu_ = view_heap_->GetGPUDescriptorHandleForHeapStart();

  D3D12_DESCRIPTOR_HEAP_DESC sampler_desc = {};
  sampler_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  sampler_desc.NumDescriptors = 2048;  // api maximum for shader-visible sampler heaps
  sampler_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&sampler_desc, IID_ID3D12DescriptorHeap,
                                           reinterpret_cast<void**>(&sampler_heap_)))) {
    REC_ERROR("d3d12: shader-visible sampler heap creation failed");
    return false;
  }
  sampler_heap_cpu_ = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
  sampler_heap_gpu_ = sampler_heap_->GetGPUDescriptorHandleForHeapStart();

  if (!srv_pool_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536) ||
      !rtv_pool_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096) ||
      !dsv_pool_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024) ||
      !sampler_pool_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256)) {
    REC_ERROR("d3d12: cpu descriptor pool creation failed");
    return false;
  }

  persistent_free_.push_back({0, kPersistentViews});

  for (u32 i = 0; i < kRingCount; ++i) {
    Ring& ring = rings_[i];
    if (FAILED(device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
            reinterpret_cast<void**>(&ring.alloc)))) {
      return false;
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ring.alloc, nullptr,
                                          IID_ID3D12GraphicsCommandList,
                                          reinterpret_cast<void**>(&ring.list)))) {
      return false;
    }
    ring.list->Close();  // rings begin recording via Reset
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                                    reinterpret_cast<void**>(&ring.fence)))) {
      return false;
    }
    ring.view_base = kPersistentViews + i * kTransientViews;

    D3D12_HEAP_PROPERTIES upload = {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC push_desc = {};
    push_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    push_desc.Width = kPushRingBytes;
    push_desc.Height = 1;
    push_desc.DepthOrArraySize = 1;
    push_desc.MipLevels = 1;
    push_desc.SampleDesc.Count = 1;
    push_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &push_desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_ID3D12Resource,
                                                reinterpret_cast<void**>(&ring.push_ring)))) {
      return false;
    }
    void* mapped = nullptr;
    ring.push_ring->Map(0, nullptr, &mapped);
    ring.push_mapped = static_cast<u8*>(mapped);
    ring.wrapper = std::make_unique<D3D12CommandList>(*this, ring.list, i);
  }
  return true;
}

void D3D12Device::ShutdownResources() {
  for (Ring& ring : rings_) {
    for (ID3D12Resource* resource : ring.deferred) resource->Release();
    ring.deferred.clear();
    SafeRelease(ring.push_ring);
    SafeRelease(ring.fence);
    SafeRelease(ring.list);
    SafeRelease(ring.alloc);
    ring.wrapper.reset();
  }
  for (auto entry : blit_pipeline_cache_) DestroyPipeline(entry.value);
  blit_pipeline_cache_.clear();
  for (auto entry : command_signature_cache_) entry.value->Release();
  command_signature_cache_.clear();
  for (auto entry : root_signature_cache_) entry.value->Release();
  root_signature_cache_.clear();
  for (auto entry : set_layout_cache_) delete entry.value;
  set_layout_cache_.clear();
  srv_pool_.Shutdown();
  rtv_pool_.Shutdown();
  dsv_pool_.Shutdown();
  sampler_pool_.Shutdown();
  SafeRelease(sampler_heap_);
  SafeRelease(view_heap_);
  SafeRelease(queue_);
}

D3D12Device::~D3D12Device() {
  if (device_) {
    WaitIdle();
    ShutdownResources();
    SafeRelease(device5_);
    SafeRelease(device2_);
    SafeRelease(device_);
  }
}

void D3D12Device::SignalAndWait() {
  Ring& ring = rings_[kImmediateRing];
  ++ring.fence_value;
  queue_->Signal(ring.fence, ring.fence_value);
  if (ring.fence->GetCompletedValue() < ring.fence_value) {
    HANDLE event = CreateFenceEvent();
    ring.fence->SetEventOnCompletion(ring.fence_value, event);
    WaitFenceEvent(event);
    DestroyFenceEvent(event);
  }
}

void D3D12Device::WaitIdle() {
  if (queue_) SignalAndWait();
}

std::unique_ptr<Swapchain> D3D12Device::CreateSwapchain(u32 width, u32 height, bool vsync,
                                                        bool /*hdr*/) {
  return D3D12Swapchain::Create(*this, width, height, vsync);
}

Device::MemoryBudget D3D12Device::memory_budget() const {
  // No DXGI budget query through vkd3d; the overlay shows zeros on linux.
  return {};
}

// --- resources ---

GpuBuffer D3D12Device::CreateBuffer(u64 size, BufferUsageFlags usage, bool host_visible) {
  bool wants_uav = (usage & (kBufferUsageStorage | kBufferUsageTransferDst |
                             kBufferUsageAccelScratch | kBufferUsageAccelStorage)) != 0;
  u64 alloc_size = size;
  if (usage & kBufferUsageUniform) alloc_size = (alloc_size + 255) & ~255ull;

  D3D12_HEAP_PROPERTIES heap = {};
  D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
  bool fixed_state = false;
  bool uav_flag = wants_uav;
  if (host_visible && (usage & kBufferUsageStorage)) {
    // CPU-written, shader-read/-written tables (bindless registry, exposure
    // readback): a CUSTOM write-back heap keeps both sides legal, which
    // UPLOAD/READBACK cannot (no UAV there).
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  } else if (host_visible && (usage & kBufferUsageTransferDst)) {
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    initial = D3D12_RESOURCE_STATE_COPY_DEST;
    fixed_state = true;
    uav_flag = false;
  } else if (host_visible) {
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    initial = D3D12_RESOURCE_STATE_GENERIC_READ;
    fixed_state = true;
    uav_flag = false;
  } else {
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  }

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = alloc_size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (uav_flag) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ID3D12Resource* resource = nullptr;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial,
                                              nullptr, IID_ID3D12Resource,
                                              reinterpret_cast<void**>(&resource)))) {
    REC_ERROR("d3d12: buffer allocation failed ({} bytes)", size);
    return {};
  }

  void* mapped = nullptr;
  if (host_visible) resource->Map(0, nullptr, &mapped);

  auto* record = new BufferRecord();
  record->resource = resource;
  record->gpu_va = resource->GetGPUVirtualAddress();
  record->size = size;
  record->fixed_state = fixed_state;
  record->allows_uav = uav_flag;
  return {.handle = MakeHandle<BufferHandle>(record),
          .size = size,
          .mapped = mapped,
          .address = (usage & (kBufferUsageDeviceAddress | kBufferUsageAccelBuildInput |
                               kBufferUsageAccelStorage | kBufferUsageAccelScratch))
                         ? record->gpu_va
                         : record->gpu_va};
}

GpuBuffer D3D12Device::CreateBufferWithData(ByteSpan data, BufferUsageFlags usage) {
  GpuBuffer staging = CreateBuffer(data.size(), kBufferUsageTransferSrc, true);
  if (!staging.mapped) return {};
  std::memcpy(staging.mapped, data.data(), data.size());

  GpuBuffer buffer = CreateBuffer(data.size(), usage | kBufferUsageTransferDst, false);
  ImmediateSubmit(
      [&](CommandList& cmd) { cmd.CopyBuffer(staging, 0, buffer, 0, data.size()); });
  DestroyBuffer(staging);
  return buffer;
}

void D3D12Device::DestroyBuffer(GpuBuffer& buffer) {
  if (BufferRecord* record = Rec(buffer.handle)) {
    SafeRelease(record->resource);
    delete record;
  }
  buffer = {};
}

TextureRecord* D3D12Device::MakeTextureRecord(ID3D12Resource* resource, Format format,
                                              Extent2D extent, u32 mips, u32 layers, bool cube,
                                              TextureUsageFlags usage) {
  auto* record = new TextureRecord();
  record->resource = resource;
  record->format = ToDxgiFormat(format);
  record->rhi_format = format;
  record->extent = extent;
  record->mip_levels = mips;
  record->array_layers = layers;
  record->cube = cube;
  record->usage = usage;
  record->sub_states.resize(static_cast<size_t>(mips) * layers, D3D12_RESOURCE_STATE_COMMON);
  record->default_view = new TextureViewRecord{record, 0, mips, false};
  return record;
}

GpuImage D3D12Device::WrapTexture(TextureRecord* record) {
  return {.handle = MakeHandle<TextureHandle>(record),
          .view = MakeHandle<TextureView>(record->default_view),
          .format = record->rhi_format,
          .extent = record->extent,
          .mip_levels = record->mip_levels};
}

GpuImage D3D12Device::CreateImage2D(Format format, Extent2D extent, TextureUsageFlags usage,
                                    u32 mip_levels) {
  bool depth = IsDepthFormat(format);
  bool block = format >= Format::kBC1RgbUnorm && format <= Format::kBC7Srgb;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = extent.width;
  desc.Height = extent.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = static_cast<UINT16>(mip_levels);
  // Depth resources are created typeless so both the DSV (D32_FLOAT/D16_UNORM)
  // and the SRV (R32_FLOAT/R16_UNORM) can view them.
  desc.Format = depth ? (format == Format::kD16Unorm ? DXGI_FORMAT_R16_TYPELESS
                                                     : DXGI_FORMAT_R32_TYPELESS)
                      : ToDxgiFormat(format);
  desc.SampleDesc.Count = 1;
  if (usage & kTextureUsageColorTarget) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (usage & kTextureUsageDepthTarget) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  if (usage & kTextureUsageStorage) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  // BlitMip (mip-chain generation) lowers to a fullscreen draw and ClearColor
  // to an RTV clear: any transfer-destination image gets render-target
  // capability so both work (BC/depth formats cannot, and don't need to).
  if ((usage & kTextureUsageTransferDst) && !depth && !block) {
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* resource = nullptr;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_ID3D12Resource,
                                              reinterpret_cast<void**>(&resource)))) {
    REC_ERROR("d3d12: image allocation failed: format {} {}x{} usage {:#x} mips {}",
              static_cast<int>(format), extent.width, extent.height, static_cast<u64>(usage),
              mip_levels);
    return {};
  }
  return WrapTexture(MakeTextureRecord(resource, format, extent, mip_levels, 1, false, usage));
}

GpuImage D3D12Device::CreateImage3D(Format format, u32 width, u32 height, u32 depth,
                                    TextureUsageFlags usage) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = static_cast<UINT16>(depth);
  desc.MipLevels = 1;
  desc.Format = ToDxgiFormat(format);
  desc.SampleDesc.Count = 1;
  if (usage & kTextureUsageStorage) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (usage & kTextureUsageTransferDst) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* resource = nullptr;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_ID3D12Resource,
                                              reinterpret_cast<void**>(&resource)))) {
    REC_ERROR("d3d12: 3d image allocation failed: format {} {}x{}x{}",
              static_cast<int>(format), width, height, depth);
    return {};
  }
  TextureRecord* record = MakeTextureRecord(resource, format, {width, height}, 1, 1, false, usage);
  record->depth = depth;
  record->volume = true;
  return WrapTexture(record);
}

GpuImage D3D12Device::CreateImageCube(Format format, u32 size, TextureUsageFlags usage,
                                      u32 mip_levels) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = size;
  desc.Height = size;
  desc.DepthOrArraySize = 6;
  desc.MipLevels = static_cast<UINT16>(mip_levels);
  desc.Format = ToDxgiFormat(format);
  desc.SampleDesc.Count = 1;
  if (usage & kTextureUsageColorTarget) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (usage & kTextureUsageStorage) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* resource = nullptr;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_ID3D12Resource,
                                              reinterpret_cast<void**>(&resource)))) {
    REC_ERROR("d3d12: cube image allocation failed");
    return {};
  }
  return WrapTexture(MakeTextureRecord(resource, format, {size, size}, mip_levels, 6, true,
                                       usage));
}

void D3D12Device::DestroyImage(GpuImage& image) {
  if (TextureRecord* record = Rec(image.handle)) {
    if (record->default_view) {
      DestroyView(MakeHandle<TextureView>(record->default_view));
      record->default_view = nullptr;
    }
    SafeRelease(record->resource);
    delete record;
  }
  image = {};
}

TextureView D3D12Device::CreateMipView(const GpuImage& image, u32 mip) {
  TextureRecord* record = Rec(image.handle);
  // Layered images (cube maps) get an array view over every layer so compute
  // passes can write all faces through one RWTexture2DArray binding.
  auto* view = new TextureViewRecord{record, mip, 1, record->array_layers > 1};
  return MakeHandle<TextureView>(view);
}

TextureView D3D12Device::CreateArrayView(const GpuImage& image) {
  TextureRecord* record = Rec(image.handle);
  auto* view = new TextureViewRecord{record, 0, record->mip_levels, true};
  return MakeHandle<TextureView>(view);
}

void D3D12Device::DestroyView(TextureView view) {
  if (TextureViewRecord* record = Rec(view)) {
    TextureRecord* texture = record->texture;
    if (texture && texture->default_view == record) texture->default_view = nullptr;
    if (record->srv != kInvalidIndex) srv_pool_.Free(record->srv);
    if (record->uav != kInvalidIndex) srv_pool_.Free(record->uav);
    if (record->rtv != kInvalidIndex) rtv_pool_.Free(record->rtv);
    if (record->dsv != kInvalidIndex) dsv_pool_.Free(record->dsv);
    delete record;
  }
}

u32 D3D12Device::EnsureSrv(TextureViewRecord* view) {
  if (view->srv != kInvalidIndex) return view->srv;
  TextureRecord* texture = view->texture;
  u32 mip_count = view->mip_count ? view->mip_count : texture->mip_levels - view->base_mip;

  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = ToDxgiSrvFormat(texture->rhi_format);
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (texture->volume) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    desc.Texture3D.MostDetailedMip = view->base_mip;
    desc.Texture3D.MipLevels = mip_count;
  } else if (texture->cube && !view->force_array) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    desc.TextureCube.MostDetailedMip = view->base_mip;
    desc.TextureCube.MipLevels = mip_count;
  } else if (texture->array_layers > 1 || view->force_array) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MostDetailedMip = view->base_mip;
    desc.Texture2DArray.MipLevels = mip_count;
    desc.Texture2DArray.ArraySize = texture->array_layers;
  } else {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = view->base_mip;
    desc.Texture2D.MipLevels = mip_count;
  }
  view->srv = srv_pool_.Alloc();
  device_->CreateShaderResourceView(texture->resource, &desc, srv_pool_.Cpu(view->srv));
  return view->srv;
}

u32 D3D12Device::EnsureUav(TextureViewRecord* view) {
  if (view->uav != kInvalidIndex) return view->uav;
  TextureRecord* texture = view->texture;

  D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.Format = ToDxgiFormat(texture->rhi_format);
  if (texture->volume) {
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    desc.Texture3D.MipSlice = view->base_mip;
    desc.Texture3D.FirstWSlice = 0;
    desc.Texture3D.WSize = texture->depth;
  } else if (texture->array_layers > 1) {
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = view->base_mip;
    desc.Texture2DArray.ArraySize = texture->array_layers;
  } else {
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = view->base_mip;
  }
  view->uav = srv_pool_.Alloc();
  device_->CreateUnorderedAccessView(texture->resource, nullptr, &desc, srv_pool_.Cpu(view->uav));
  return view->uav;
}

u32 D3D12Device::EnsureRtv(TextureViewRecord* view) {
  if (view->rtv != kInvalidIndex) return view->rtv;
  TextureRecord* texture = view->texture;
  D3D12_RENDER_TARGET_VIEW_DESC desc = {};
  desc.Format = ToDxgiFormat(texture->rhi_format);
  if (texture->array_layers > 1) {
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = view->base_mip;
    desc.Texture2DArray.ArraySize = texture->array_layers;
  } else {
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = view->base_mip;
  }
  view->rtv = rtv_pool_.Alloc();
  device_->CreateRenderTargetView(texture->resource, &desc, rtv_pool_.Cpu(view->rtv));
  return view->rtv;
}

u32 D3D12Device::EnsureDsv(TextureViewRecord* view) {
  if (view->dsv != kInvalidIndex) return view->dsv;
  TextureRecord* texture = view->texture;
  D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
  desc.Format = texture->rhi_format == Format::kD16Unorm ? DXGI_FORMAT_D16_UNORM
                                                         : DXGI_FORMAT_D32_FLOAT;
  desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipSlice = view->base_mip;
  view->dsv = dsv_pool_.Alloc();
  device_->CreateDepthStencilView(texture->resource, &desc, dsv_pool_.Cpu(view->dsv));
  return view->dsv;
}

SamplerHandle D3D12Device::GetSampler(const SamplerDesc& desc) {
  u64 key = HashBytes(&desc, sizeof(desc));
  if (u64* cached = sampler_cache_.find(key)) return SamplerHandle{*cached};

  D3D12_SAMPLER_DESC sampler = {};
  sampler.Filter = ToFilter(desc);
  sampler.AddressU = ToAddressMode(desc.address_u);
  sampler.AddressV = ToAddressMode(desc.address_v);
  sampler.AddressW = ToAddressMode(desc.address_w);
  if (desc.max_anisotropy > 1.0f) {
    sampler.MaxAnisotropy = static_cast<UINT>(std::min(desc.max_anisotropy, 16.0f));
  }
  sampler.MinLOD = desc.min_lod;
  sampler.MaxLOD = desc.max_lod;
  sampler.ComparisonFunc =
      desc.compare_enable ? ToCompareFunc(desc.compare_op) : D3D12_COMPARISON_FUNC_NEVER;
  switch (desc.border) {
    case BorderColor::kTransparentBlack: break;  // zeros
    case BorderColor::kOpaqueBlack: sampler.BorderColor[3] = 1.0f; break;
    case BorderColor::kOpaqueWhite:
      sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] =
          sampler.BorderColor[3] = 1.0f;
      break;
  }

  u32 index = sampler_pool_.Alloc();
  device_->CreateSampler(&sampler, sampler_pool_.Cpu(index));
  u64 value = static_cast<u64>(index) + 1;  // 0 stays "null handle"
  sampler_cache_.insert(key, value);
  return SamplerHandle{value};
}

// --- bindings ---

SetLayout* D3D12Device::GetOrCreateSetLayout(const BindingLayoutDesc& desc) {
  u64 key = HashLayoutDesc(desc);
  if (SetLayout** cached = set_layout_cache_.find(key)) return *cached;

  auto* layout = new SetLayout();
  layout->hash = key;
  for (const BindingSlot& slot : desc.slots) {
    SetLayout::Slot entry;
    entry.slot = slot;
    switch (slot.type) {
      case BindingType::kStorageImage:
      case BindingType::kSampledImage:
      case BindingType::kUniformBuffer:
      case BindingType::kAccelStruct:
        entry.view_offset = layout->view_count;
        layout->view_count += slot.count;
        break;
      case BindingType::kStorageBuffer:
        // Two descriptors: SRV (StructuredBuffer) then UAV (RWStructuredBuffer);
        // the shader picks its class, the root signature declares both.
        entry.view_offset = layout->view_count;
        layout->view_count += 2 * slot.count;
        break;
      case BindingType::kCombinedTextureSampler:
        entry.view_offset = layout->view_count;
        layout->view_count += slot.count;
        entry.sampler_offset = layout->sampler_count;
        layout->sampler_count += slot.count;
        break;
      case BindingType::kSampler:
        entry.sampler_offset = layout->sampler_count;
        layout->sampler_count += slot.count;
        break;
    }
    layout->slots.push_back(entry);
  }
  set_layout_cache_.insert(key, layout);
  return layout;
}

ID3D12RootSignature* D3D12Device::GetOrCreateRootSignature(std::span<SetLayout* const> sets,
                                                           u32 push_size,
                                                           bool push_root_constants,
                                                           PipelineRecord* out) {
  u64 key = HashBytes(sets.data(), sets.size() * sizeof(SetLayout*));
  key = HashBytes(&push_size, sizeof(push_size), key);

  // Root parameter layout: [push] then per set a view table and a sampler
  // table (each only when non-empty). Recorded into the pipeline record.
  base::Vector<D3D12_ROOT_PARAMETER> params;
  base::Vector<base::Vector<D3D12_DESCRIPTOR_RANGE>> range_storage;
  out->sets.clear();
  out->push_param = -1;

  if (push_size > 0) {
    D3D12_ROOT_PARAMETER param = {};
    if (push_root_constants) {
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      param.Constants.ShaderRegister = 999;
      param.Constants.RegisterSpace = 0;
      param.Constants.Num32BitValues = (push_size + 3) / 4;
    } else {
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      param.Descriptor.ShaderRegister = 999;
      param.Descriptor.RegisterSpace = 0;
    }
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out->push_param = static_cast<i32>(params.size());
    params.push_back(param);
  }

  for (u32 set = 0; set < sets.size(); ++set) {
    const SetLayout* layout = sets[set];
    PipelineRecord::SetParams set_params;
    set_params.layout = const_cast<SetLayout*>(layout);

    base::Vector<D3D12_DESCRIPTOR_RANGE> view_ranges;
    base::Vector<D3D12_DESCRIPTOR_RANGE> sampler_ranges;
    for (const SetLayout::Slot& slot : layout->slots) {
      auto range = [&](D3D12_DESCRIPTOR_RANGE_TYPE type, u32 count, u32 offset) {
        return D3D12_DESCRIPTOR_RANGE{type, count, slot.slot.binding, set, offset};
      };
      switch (slot.slot.type) {
        case BindingType::kStorageImage:
          view_ranges.push_back(
              range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, slot.slot.count, slot.view_offset));
          break;
        case BindingType::kSampledImage:
        case BindingType::kAccelStruct:
          view_ranges.push_back(
              range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, slot.slot.count, slot.view_offset));
          break;
        case BindingType::kUniformBuffer:
          view_ranges.push_back(
              range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, slot.slot.count, slot.view_offset));
          break;
        case BindingType::kStorageBuffer:
          // SRV/UAV descriptor pairs are interleaved per array element.
          for (u32 i = 0; i < slot.slot.count; ++i) {
            view_ranges.push_back(
                range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, slot.view_offset + 2 * i));
            view_ranges.back().BaseShaderRegister = slot.slot.binding + i;
            view_ranges.push_back(
                range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, slot.view_offset + 2 * i + 1));
            view_ranges.back().BaseShaderRegister = slot.slot.binding + i;
          }
          break;
        case BindingType::kCombinedTextureSampler:
          view_ranges.push_back(
              range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, slot.slot.count, slot.view_offset));
          sampler_ranges.push_back(
              range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, slot.slot.count, slot.sampler_offset));
          break;
        case BindingType::kSampler:
          sampler_ranges.push_back(
              range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, slot.slot.count, slot.sampler_offset));
          break;
      }
    }

    if (!view_ranges.empty()) {
      range_storage.push_back(std::move(view_ranges));
      D3D12_ROOT_PARAMETER param = {};
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      param.DescriptorTable.NumDescriptorRanges = static_cast<u32>(range_storage.back().size());
      param.DescriptorTable.pDescriptorRanges = range_storage.back().data();
      param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      set_params.view_param = static_cast<i32>(params.size());
      params.push_back(param);
    }
    if (!sampler_ranges.empty()) {
      range_storage.push_back(std::move(sampler_ranges));
      D3D12_ROOT_PARAMETER param = {};
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      param.DescriptorTable.NumDescriptorRanges = static_cast<u32>(range_storage.back().size());
      param.DescriptorTable.pDescriptorRanges = range_storage.back().data();
      param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      set_params.sampler_param = static_cast<i32>(params.size());
      params.push_back(param);
    }
    out->sets.push_back(set_params);
  }

  // Push-block buffer-device-address emulation (skinned bone palettes): a
  // root SRV at (t998, space0). Shaders that do not declare it simply leave
  // the parameter unused; the command list binds it from the address at push
  // byte 128 whenever the push block is large enough to carry one.
  {
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param.Descriptor.ShaderRegister = 998;
    param.Descriptor.RegisterSpace = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out->bda_param = static_cast<i32>(params.size());
    params.push_back(param);
  }

  if (ID3D12RootSignature** cached = root_signature_cache_.find(key)) return *cached;

  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = static_cast<u32>(params.size());
  desc.pParameters = params.data();
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ID3DBlob* blob = nullptr;
  ID3DBlob* error = nullptr;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error))) {
    REC_ERROR("d3d12: root signature serialization failed: {}",
              error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    if (error) error->Release();
    return nullptr;
  }
  ID3D12RootSignature* root = nullptr;
  HRESULT hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_ID3D12RootSignature,
                                            reinterpret_cast<void**>(&root));
  blob->Release();
  if (FAILED(hr)) {
    REC_ERROR("d3d12: root signature creation failed");
    return nullptr;
  }
  root_signature_cache_.insert(key, root);
  return root;
}

BindingLayoutHandle D3D12Device::CreateBindingLayout(const BindingLayoutDesc& desc) {
  SetLayout* layout = GetOrCreateSetLayout(desc);
  auto* record = new BindingLayoutRecord{layout};
  return MakeHandle<BindingLayoutHandle>(record);
}

void D3D12Device::DestroyBindingLayout(BindingLayoutHandle layout) {
  delete Rec(layout);  // SetLayout itself is cached, freed at shutdown
}

BindingSetHandle D3D12Device::CreateBindingSet(BindingLayoutHandle layout, u32 variable_count) {
  (void)variable_count;  // arrays are fixed-size (layout-declared count)
  SetLayout* set_layout = Rec(layout)->layout;

  u32 view_start = kInvalidIndex;
  if (set_layout->view_count > 0) {
    for (size_t i = 0; i < persistent_free_.size(); ++i) {
      if (persistent_free_[i].count >= set_layout->view_count) {
        view_start = persistent_free_[i].start;
        persistent_free_[i].start += set_layout->view_count;
        persistent_free_[i].count -= set_layout->view_count;
        break;
      }
    }
    if (view_start == kInvalidIndex) {
      REC_ERROR("d3d12: persistent descriptor region exhausted");
      return {};
    }
    WriteNullViewDescriptors(*set_layout, view_start);
  }

  u32 sampler_start = kInvalidIndex;
  if (set_layout->sampler_count > 0) {
    if (sampler_persistent_next_ + set_layout->sampler_count > 2048) {
      REC_ERROR("d3d12: shader-visible sampler heap exhausted");
      return {};
    }
    sampler_start = sampler_persistent_next_;
    sampler_persistent_next_ += set_layout->sampler_count;
  }

  auto* record = new BindingSetRecord{set_layout, view_start, sampler_start};
  return MakeHandle<BindingSetHandle>(record);
}

void D3D12Device::DestroyBindingSet(BindingSetHandle set) {
  if (BindingSetRecord* record = Rec(set)) {
    if (record->view_start != kInvalidIndex) {
      persistent_free_.push_back({record->view_start, record->layout->view_count});
    }
    // Sampler regions are bump-allocated and few; not recycled.
    delete record;
  }
}

void D3D12Device::WriteSamplerDescriptor(u32 heap_index, SamplerHandle sampler) {
  if (!sampler) return;
  u32 pool_index = static_cast<u32>(sampler.value - 1);
  device_->CopyDescriptorsSimple(1, SamplerCpuVisible(heap_index), sampler_pool_.Cpu(pool_index),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
}

void D3D12Device::WriteNullViewDescriptors(const SetLayout& layout, u32 table_start) {
  for (const SetLayout::Slot& slot : layout.slots) {
    if (slot.view_offset == kInvalidIndex) continue;
    for (u32 i = 0; i < slot.slot.count; ++i) {
      switch (slot.slot.type) {
        case BindingType::kSampledImage:
        case BindingType::kCombinedTextureSampler:
        case BindingType::kAccelStruct: {
          D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
          desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          desc.Texture2D.MipLevels = 1;
          device_->CreateShaderResourceView(nullptr, &desc,
                                            ViewCpu(table_start + slot.view_offset + i));
          break;
        }
        case BindingType::kStorageImage: {
          D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
          desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
          device_->CreateUnorderedAccessView(nullptr, nullptr, &desc,
                                             ViewCpu(table_start + slot.view_offset + i));
          break;
        }
        case BindingType::kUniformBuffer: {
          D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
          device_->CreateConstantBufferView(&desc, ViewCpu(table_start + slot.view_offset + i));
          break;
        }
        case BindingType::kStorageBuffer: {
          D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
          srv.Format = DXGI_FORMAT_R32_TYPELESS;
          srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
          device_->CreateShaderResourceView(nullptr, &srv,
                                            ViewCpu(table_start + slot.view_offset + 2 * i));
          D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
          uav.Format = DXGI_FORMAT_R32_TYPELESS;
          uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
          uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
          device_->CreateUnorderedAccessView(nullptr, nullptr, &uav,
                                             ViewCpu(table_start + slot.view_offset + 2 * i + 1));
          break;
        }
        case BindingType::kSampler: break;
      }
    }
  }
}

void D3D12Device::WriteViewDescriptor(const SetLayout::Slot& slot, const BindingItem& item,
                                      u32 table_start) {
  u32 index = table_start + slot.view_offset + item.array_index *
                                                   (slot.slot.type == BindingType::kStorageBuffer
                                                        ? 2u
                                                        : 1u);
  switch (item.type) {
    case BindingType::kStorageImage: {
      TextureViewRecord* view = Rec(item.view);
      device_->CopyDescriptorsSimple(1, ViewCpu(index), srv_pool_.Cpu(EnsureUav(view)),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      break;
    }
    case BindingType::kSampledImage:
    case BindingType::kCombinedTextureSampler: {
      TextureViewRecord* view = Rec(item.view);
      device_->CopyDescriptorsSimple(1, ViewCpu(index), srv_pool_.Cpu(EnsureSrv(view)),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      break;
    }
    case BindingType::kUniformBuffer: {
      BufferRecord* buffer = Rec(item.buffer);
      u64 range = item.buffer_range ? item.buffer_range : buffer->size - item.buffer_offset;
      D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
      desc.BufferLocation = buffer->gpu_va + item.buffer_offset;
      desc.SizeInBytes = static_cast<u32>((range + 255) & ~255ull);
      device_->CreateConstantBufferView(&desc, ViewCpu(index));
      break;
    }
    case BindingType::kStorageBuffer: {
      BufferRecord* buffer = Rec(item.buffer);
      u64 range = item.buffer_range ? item.buffer_range : buffer->size - item.buffer_offset;
      // Raw (ByteAddressBuffer-shaped) views: stride-agnostic, which matches
      // vkd3d's byte-range lowering of structured access. Native Windows
      // hardware that consumes the descriptor stride would need the rhi to
      // carry element strides -- documented Windows-pending work in RHI.md.
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Format = DXGI_FORMAT_R32_TYPELESS;
      srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Buffer.FirstElement = item.buffer_offset / 4;
      srv.Buffer.NumElements = static_cast<u32>(range / 4);
      srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
      device_->CreateShaderResourceView(buffer->resource, &srv, ViewCpu(index));
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
      uav.Format = DXGI_FORMAT_R32_TYPELESS;
      uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      uav.Buffer.FirstElement = item.buffer_offset / 4;
      uav.Buffer.NumElements = static_cast<u32>(range / 4);
      uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      device_->CreateUnorderedAccessView(buffer->allows_uav ? buffer->resource : nullptr, nullptr,
                                         &uav, ViewCpu(index + 1));
      break;
    }
    case BindingType::kAccelStruct: {
      D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
      desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
      desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      desc.RaytracingAccelerationStructure.Location = Rec(item.accel)->address;
      device_->CreateShaderResourceView(nullptr, &desc, ViewCpu(index));
      break;
    }
    case BindingType::kSampler: break;  // sampler table path
  }
}

void D3D12Device::UpdateBindingSet(BindingSetHandle set, std::span<const BindingItem> items) {
  BindingSetRecord* record = Rec(set);
  for (const BindingItem& item : items) {
    const SetLayout::Slot* slot = record->layout->Find(item.slot);
    if (!slot) continue;
    if (item.type == BindingType::kSampler ||
        item.type == BindingType::kCombinedTextureSampler) {
      WriteSamplerDescriptor(record->sampler_start + slot->sampler_offset + item.array_index,
                             item.sampler);
    }
    if (item.type != BindingType::kSampler) {
      WriteViewDescriptor(*slot, item, record->view_start);
    }
  }
}

u32 D3D12Device::AllocTransientViews(u32 ring_index, u32 count) {
  Ring& ring = rings_[ring_index];
  if (ring.view_cursor + count > kTransientViews) {
    if (!ring.warned_views) {
      REC_ERROR("d3d12: transient descriptor ring exhausted ({} views), wrapping", kTransientViews);
      ring.warned_views = true;
    }
    ring.view_cursor = 0;
  }
  u32 start = ring.view_base + ring.view_cursor;
  ring.view_cursor += count;
  return start;
}

u64 D3D12Device::AllocPushSlice(u32 ring_index, u32 size, void** cpu) {
  Ring& ring = rings_[ring_index];
  u64 aligned = (ring.push_cursor + 255) & ~255ull;
  if (aligned + size > kPushRingBytes) aligned = 0;  // wrap (sized generously)
  ring.push_cursor = aligned + size;
  *cpu = ring.push_mapped + aligned;
  return ring.push_ring->GetGPUVirtualAddress() + aligned;
}

u32 D3D12Device::GetSamplerTable(const u64* samplers, u32 count) {
  u64 key = HashBytes(samplers, count * sizeof(u64));
  if (u32* cached = sampler_table_cache_.find(key)) return *cached;
  if (sampler_persistent_next_ + count > 2048) {
    REC_ERROR("d3d12: shader-visible sampler heap exhausted");
    return 0;
  }
  u32 start = sampler_persistent_next_;
  sampler_persistent_next_ += count;
  for (u32 i = 0; i < count; ++i) {
    WriteSamplerDescriptor(start + i, SamplerHandle{samplers[i]});
  }
  sampler_table_cache_.insert(key, start);
  return start;
}

// --- pipelines ---

namespace {

bool ResolveSetLayouts(D3D12Device& device, const base::Vector<PipelineBindings>& sets,
                       ShaderStageFlags stages, base::Vector<SetLayout*>* out) {
  for (const PipelineBindings& set : sets) {
    if (set.shared) {
      out->push_back(Rec(set.shared)->layout);
      continue;
    }
    BindingLayoutDesc desc;
    desc.stages = set.stages ? set.stages : stages;
    desc.slots = set.slots;
    SetLayout* layout = device.GetOrCreateSetLayout(desc);
    if (!layout) return false;
    out->push_back(layout);
  }
  return true;
}

}  // namespace

PipelineHandle D3D12Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
  if (!desc.shader.dxil_valid()) {
    REC_ERROR("d3d12: no dxil sidecar for compute pipeline ({})",
              desc.debug_name ? desc.debug_name : "?");
    return {};
  }
  base::Vector<SetLayout*> set_layouts;
  if (!ResolveSetLayouts(*this, desc.sets, kShaderStageCompute, &set_layouts)) return {};

  auto* record = new PipelineRecord();
  record->compute = true;
  record->push_size = desc.push_constant_size;
  record->push_root_constants = desc.push_constant_size <= kMaxRootConstantBytes;
  record->root = GetOrCreateRootSignature({set_layouts.data(), set_layouts.size()},
                                          desc.push_constant_size, record->push_root_constants,
                                          record);
  if (!record->root) {
    delete record;
    return {};
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = record->root;
  pso.CS = {desc.shader.dxil_data, desc.shader.dxil_size};
  if (FAILED(device_->CreateComputePipelineState(&pso, IID_ID3D12PipelineState,
                                                 reinterpret_cast<void**>(&record->pso)))) {
    REC_ERROR("d3d12: compute pipeline creation failed ({})",
              desc.debug_name ? desc.debug_name : "?");
    delete record;
    return {};
  }
  record->bda_param = -1;  // no compute shader uses the push-BDA convention
  return MakeHandle<PipelineHandle>(record);
}

PipelineHandle D3D12Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
  bool mesh_path = desc.mesh.valid();
  ShaderStageFlags all_stages =
      mesh_path ? (kShaderStageTask | kShaderStageMesh | kShaderStageFragment)
                : (kShaderStageVertex | kShaderStageFragment);
  const char* name = desc.debug_name ? desc.debug_name : "?";

  if (mesh_path && (!caps_.mesh_shaders || !device2_)) {
    REC_WARN("d3d12: mesh-shader pipeline '{}' requested without mesh shader support", name);
    return {};
  }
  if (mesh_path ? !desc.mesh.dxil_valid()
                : (!desc.vertex.dxil_valid() ||
                   (desc.fragment.valid() && !desc.fragment.dxil_valid()))) {
    REC_ERROR("d3d12: no dxil sidecar for graphics pipeline ({})", name);
    return {};
  }

  base::Vector<SetLayout*> set_layouts;
  if (!ResolveSetLayouts(*this, desc.sets, all_stages, &set_layouts)) return {};

  auto* record = new PipelineRecord();
  record->compute = false;
  record->topology = DrawTopology(desc.topology);
  record->push_size = desc.push_constant_size;
  record->push_root_constants = desc.push_constant_size <= kMaxRootConstantBytes;
  record->root = GetOrCreateRootSignature({set_layouts.data(), set_layouts.size()},
                                          desc.push_constant_size, record->push_root_constants,
                                          record);
  if (!record->root) {
    delete record;
    return {};
  }
  for (const VertexBufferLayout& buffer : desc.vertex_buffers) {
    record->vertex_strides.push_back(buffer.stride);
  }

  D3D12_BLEND_DESC blend = BuildBlendDesc(desc.color_formats, desc.blend);
  D3D12_RASTERIZER_DESC raster = BuildRasterDesc(desc.raster, desc.depth);
  D3D12_DEPTH_STENCIL_DESC depth = BuildDepthDesc(desc.depth);

  // The push-BDA root SRV (bone palette) only applies to the skinned vertex
  // shaders, which are exactly the ones consuming a BLENDINDICES stream.
  // Everything else must leave the parameter unbound: binding a garbage
  // "address" read out of an unrelated push block would fault in vkd3d's VA
  // resolver at record time.
  bool uses_bone_palette = false;

  HRESULT hr;
  if (mesh_path) {
    struct {
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>
          root;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>
          raster;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>
          depth;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
                      D3D12_PRIMITIVE_TOPOLOGY_TYPE>
          topology;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
                      D3D12_RT_FORMAT_ARRAY>
          rtv_formats;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
      StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    } stream;
    stream.root.value = record->root;
    if (desc.task.dxil_valid()) stream.as.value = {desc.task.dxil_data, desc.task.dxil_size};
    stream.ms.value = {desc.mesh.dxil_data, desc.mesh.dxil_size};
    if (desc.fragment.dxil_valid()) {
      stream.ps.value = {desc.fragment.dxil_data, desc.fragment.dxil_size};
    }
    stream.blend.value = blend;
    stream.sample_mask.value = ~0u;
    stream.raster.value = raster;
    stream.depth.value = depth;
    stream.topology.value = TopologyType(desc.topology);
    stream.rtv_formats.value.NumRenderTargets = static_cast<u32>(desc.color_formats.size());
    for (size_t i = 0; i < desc.color_formats.size() && i < 8; ++i) {
      stream.rtv_formats.value.RTFormats[i] = ToDxgiFormat(desc.color_formats[i]);
    }
    stream.dsv.value = desc.depth.format != Format::kUnknown
                           ? ToDxgiFormat(desc.depth.format)
                           : DXGI_FORMAT_UNKNOWN;
    stream.sample.value = {1, 0};

    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = {sizeof(stream), &stream};
    hr = device2_->CreatePipelineState(&stream_desc, IID_ID3D12PipelineState,
                                       reinterpret_cast<void**>(&record->pso));
  } else {
    // Input layout: semantics come from the DXIL input signature (see
    // ParseInputSignature); attributes pair with signature elements in
    // location order.
    base::Vector<SignatureElement> signature;
    if (!desc.vertex_buffers.empty() &&
        !ParseInputSignature(desc.vertex.dxil_data, desc.vertex.dxil_size, &signature)) {
      REC_ERROR("d3d12: vertex input signature parse failed ({})", name);
      delete record;
      return {};
    }
    struct AttributeRef {
      const VertexAttribute* attribute;
      u32 buffer;
    };
    base::Vector<AttributeRef> attributes;
    for (u32 binding = 0; binding < desc.vertex_buffers.size(); ++binding) {
      for (const VertexAttribute& attribute : desc.vertex_buffers[binding].attributes) {
        attributes.push_back({&attribute, binding});
      }
    }
    std::sort(attributes.begin(), attributes.end(),
              [](const AttributeRef& a, const AttributeRef& b) {
                return a.attribute->location < b.attribute->location;
              });
    std::sort(signature.begin(), signature.end(),
              [](const SignatureElement& a, const SignatureElement& b) { return a.reg < b.reg; });
    if (attributes.size() != signature.size()) {
      REC_ERROR("d3d12: vertex attribute count {} != shader input count {} ({})",
                attributes.size(), signature.size(), name);
      delete record;
      return {};
    }
    base::Vector<D3D12_INPUT_ELEMENT_DESC> elements;
    for (const SignatureElement& element : signature) {
      if (element.name == "BLENDINDICES") uses_bone_palette = true;
    }
    for (size_t i = 0; i < attributes.size(); ++i) {
      D3D12_INPUT_ELEMENT_DESC element = {};
      element.SemanticName = signature[i].name.c_str();
      element.SemanticIndex = signature[i].semantic_index;
      element.Format = ToDxgiFormat(attributes[i].attribute->format);
      element.InputSlot = attributes[i].buffer;
      element.AlignedByteOffset = attributes[i].attribute->offset;
      bool per_instance = desc.vertex_buffers[attributes[i].buffer].per_instance;
      element.InputSlotClass = per_instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      element.InstanceDataStepRate = per_instance ? 1 : 0;
      elements.push_back(element);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = record->root;
    pso.VS = {desc.vertex.dxil_data, desc.vertex.dxil_size};
    if (desc.fragment.dxil_valid()) {
      pso.PS = {desc.fragment.dxil_data, desc.fragment.dxil_size};
    }
    pso.BlendState = blend;
    pso.SampleMask = ~0u;
    pso.RasterizerState = raster;
    pso.DepthStencilState = depth;
    pso.InputLayout = {elements.data(), static_cast<u32>(elements.size())};
    pso.PrimitiveTopologyType = TopologyType(desc.topology);
    pso.NumRenderTargets = static_cast<u32>(desc.color_formats.size());
    for (size_t i = 0; i < desc.color_formats.size() && i < 8; ++i) {
      pso.RTVFormats[i] = ToDxgiFormat(desc.color_formats[i]);
    }
    pso.DSVFormat = desc.depth.format != Format::kUnknown ? ToDxgiFormat(desc.depth.format)
                                                          : DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc = {1, 0};
    hr = device_->CreateGraphicsPipelineState(&pso, IID_ID3D12PipelineState,
                                              reinterpret_cast<void**>(&record->pso));
  }

  if (FAILED(hr)) {
    REC_ERROR("d3d12: graphics pipeline creation failed ({})", name);
    delete record;
    return {};
  }
  if (!uses_bone_palette) record->bda_param = -1;
  return MakeHandle<PipelineHandle>(record);
}

void D3D12Device::DestroyPipeline(PipelineHandle pipeline) {
  if (PipelineRecord* record = Rec(pipeline)) {
    SafeRelease(record->pso);
    delete record;  // root signature is cached, freed at shutdown
  }
}

ID3D12CommandSignature* D3D12Device::GetDrawIndexedSignature(u32 stride) {
  u64 key = stride;
  if (ID3D12CommandSignature** cached = command_signature_cache_.find(key)) return *cached;
  D3D12_INDIRECT_ARGUMENT_DESC argument = {};
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  D3D12_COMMAND_SIGNATURE_DESC desc = {};
  desc.ByteStride = stride;
  desc.NumArgumentDescs = 1;
  desc.pArgumentDescs = &argument;
  ID3D12CommandSignature* signature = nullptr;
  if (FAILED(device_->CreateCommandSignature(&desc, nullptr, IID_ID3D12CommandSignature,
                                             reinterpret_cast<void**>(&signature)))) {
    REC_ERROR("d3d12: command signature creation failed (stride {})", stride);
    return nullptr;
  }
  command_signature_cache_.insert(key, signature);
  return signature;
}

PipelineHandle D3D12Device::GetBlitPipeline(Format format) {
  u64 key = static_cast<u64>(format);
  if (PipelineHandle* cached = blit_pipeline_cache_.find(key)) return *cached;
  GraphicsPipelineDesc desc;
  desc.vertex = REC_SHADER(k_fullscreen_vs_hlsl);
  desc.fragment = REC_SHADER(k_blit_ps_hlsl);
  desc.raster.cull = CullMode::kNone;
  desc.color_formats.push_back(format);
  PipelineBindings set;
  set.slots.push_back({0, BindingType::kCombinedTextureSampler});
  desc.sets.push_back(std::move(set));
  desc.debug_name = "blit_mip";
  PipelineHandle pipeline = CreateGraphicsPipeline(desc);
  blit_pipeline_cache_.insert(key, pipeline);
  return pipeline;
}

SamplerHandle D3D12Device::blit_sampler() {
  if (!blit_sampler_) {
    blit_sampler_ = GetSampler({.address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge,
                                .address_w = AddressMode::kClampToEdge});
  }
  return blit_sampler_;
}

void D3D12Device::DeferRelease(u32 ring, ID3D12Resource* resource) {
  rings_[ring].deferred.push_back(resource);
}

// --- acceleration structures ---

namespace {

DXGI_FORMAT AccelVertexFormat(Format format) {
  return format == Format::kRGB32Float ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32_FLOAT;
}

void FillGeometries(const BlasBuildDesc& desc,
                    base::Vector<D3D12_RAYTRACING_GEOMETRY_DESC>* out) {
  for (const AccelTriangles& t : desc.geometries) {
    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = t.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                              : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geometry.Triangles.VertexBuffer = {t.vertex_address, t.vertex_stride};
    geometry.Triangles.VertexFormat = AccelVertexFormat(t.vertex_format);
    geometry.Triangles.VertexCount = t.vertex_count;
    geometry.Triangles.IndexBuffer = t.index_address;
    geometry.Triangles.IndexCount = t.index_count;
    geometry.Triangles.IndexFormat =
        t.index_type == IndexType::kUint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    out->push_back(geometry);
  }
}

}  // namespace

AccelSizes D3D12Device::GetBlasSizes(const BlasBuildDesc& desc) {
  if (!device5_) return {};
  base::Vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
  FillGeometries(desc, &geometries);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags = desc.fast_trace
                     ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                     : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  inputs.NumDescs = static_cast<u32>(geometries.size());
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.pGeometryDescs = geometries.data();

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
  device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  return {info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes};
}

AccelSizes D3D12Device::GetTlasSizes(u32 instance_count) {
  if (!device5_) return {};
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  inputs.NumDescs = instance_count;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
  device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  return {info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes};
}

AccelStructHandle D3D12Device::CreateAccelStruct(AccelStructType type, u64 size) {
  (void)type;  // both levels are plain UAV buffers in the AS state
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ID3D12Resource* resource = nullptr;
  if (FAILED(device_->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_ID3D12Resource,
          reinterpret_cast<void**>(&resource)))) {
    REC_ERROR("d3d12: acceleration structure allocation failed ({} bytes)", size);
    return {};
  }
  auto* record = new AccelStructRecord{resource, resource->GetGPUVirtualAddress()};
  return MakeHandle<AccelStructHandle>(record);
}

void D3D12Device::DestroyAccelStruct(AccelStructHandle accel) {
  if (AccelStructRecord* record = Rec(accel)) {
    SafeRelease(record->resource);
    delete record;
  }
}

u64 D3D12Device::accel_address(AccelStructHandle accel) { return Rec(accel)->address; }

// --- profiling ---

TimestampPoolHandle D3D12Device::CreateTimestampPool(u32 count) {
  D3D12_QUERY_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  heap_desc.Count = count;
  ID3D12QueryHeap* heap = nullptr;
  if (FAILED(device_->CreateQueryHeap(&heap_desc, IID_ID3D12QueryHeap,
                                      reinterpret_cast<void**>(&heap)))) {
    return {};
  }
  D3D12_HEAP_PROPERTIES readback = {};
  readback.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = static_cast<u64>(count) * sizeof(u64);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* buffer = nullptr;
  if (FAILED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_ID3D12Resource,
                                              reinterpret_cast<void**>(&buffer)))) {
    heap->Release();
    return {};
  }
  void* mapped = nullptr;
  buffer->Map(0, nullptr, &mapped);
  auto* record = new TimestampPoolRecord{heap, buffer, mapped, count};
  return MakeHandle<TimestampPoolHandle>(record);
}

void D3D12Device::DestroyTimestampPool(TimestampPoolHandle pool) {
  if (TimestampPoolRecord* record = Rec(pool)) {
    SafeRelease(record->readback);
    SafeRelease(record->heap);
    delete record;
  }
}

bool D3D12Device::GetTimestamps(TimestampPoolHandle pool, u32 first, u32 count, u64* out) {
  TimestampPoolRecord* record = Rec(pool);
  if (!record->mapped || first + count > record->count) return false;
  std::memcpy(out, static_cast<const u64*>(record->mapped) + first, count * sizeof(u64));
  return true;
}

// --- recording & submission ---

D3D12CommandList* D3D12Device::BeginRing(u32 ring_index) {
  Ring& ring = rings_[ring_index];
  for (ID3D12Resource* resource : ring.deferred) resource->Release();
  ring.deferred.clear();
  ring.alloc->Reset();
  ring.list->Reset(ring.alloc, nullptr);
  ID3D12DescriptorHeap* heaps[] = {view_heap_, sampler_heap_};
  ring.list->SetDescriptorHeaps(2, heaps);
  ring.view_cursor = 0;
  ring.push_cursor = 0;
  ring.wrapper->OnBeginRecording();
  return ring.wrapper.get();
}

void D3D12Device::CloseAndExecute(u32 ring_index) {
  Ring& ring = rings_[ring_index];
  ring.wrapper->OnEndRecording();
  ring.list->Close();
  ID3D12CommandList* lists[] = {ring.list};
  queue_->ExecuteCommandLists(1, lists);
  ++ring.fence_value;
  queue_->Signal(ring.fence, ring.fence_value);
}

void D3D12Device::ImmediateSubmit(const std::function<void(CommandList&)>& record) {
  Ring& ring = rings_[kImmediateRing];
  D3D12CommandList* cmd = BeginRing(kImmediateRing);
  record(*cmd);
  CloseAndExecute(kImmediateRing);
  if (ring.fence->GetCompletedValue() < ring.fence_value) {
    HANDLE event = CreateFenceEvent();
    ring.fence->SetEventOnCompletion(ring.fence_value, event);
    WaitFenceEvent(event);
    DestroyFenceEvent(event);
  }
  for (ID3D12Resource* resource : ring.deferred) resource->Release();
  ring.deferred.clear();
}

CommandList* D3D12Device::BeginFrame(u32 slot) {
  Ring& ring = rings_[slot];
  if (ring.fence_value > 0 && ring.fence->GetCompletedValue() < ring.fence_value) {
    HANDLE event = CreateFenceEvent();
    ring.fence->SetEventOnCompletion(ring.fence_value, event);
    WaitFenceEvent(event);
    DestroyFenceEvent(event);
  }
  ++frame_epoch_;
  return BeginRing(slot);
}

PresentResult D3D12Device::SubmitFrame(CommandList* cmd, Swapchain& swapchain, u32 image_index) {
  (void)image_index;  // the swapchain tracked it at Acquire
  for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
    if (rings_[i].wrapper.get() == cmd) {
      CloseAndExecute(i);
      // DXGI Present on windows; no-op against the offscreen ring on linux.
      return static_cast<D3D12Swapchain&>(swapchain).Present();
    }
  }
  return PresentResult::kFailed;
}

}  // namespace rec::render::d3d12

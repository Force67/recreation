// D3D12 swapchain.
//
// Windows: DXGI flip-model over the SDL window's HWND. Compile-guarded and
// still pending runtime validation on a Windows machine (tracked in RHI.md);
// the structure mirrors the offscreen path so the rest of the backend is
// identical between the two.
//
// Linux/vkd3d: there is no DXGI, so the baseline is an offscreen ring of
// three presentable render targets. Acquire cycles them, SubmitFrame's fence
// provides the pacing and "present" is a no-op. The whole frame graph (and
// REC_UI_SHOT screenshot readback) runs unchanged against the ring.

#include "render/d3d12/d3d12_backend.h"

#include "core/log.h"

namespace rec::render::d3d12 {

std::unique_ptr<D3D12Swapchain> D3D12Swapchain::Create(D3D12Device& device, u32 width, u32 height,
                                                       bool vsync) {
  auto swapchain = std::unique_ptr<D3D12Swapchain>(new D3D12Swapchain(device));
  if (!swapchain->Init(width, height, vsync)) return nullptr;
  return swapchain;
}

#if defined(_WIN32)

// SDL3 window-property access, declared locally so the render module does not
// grow an SDL include dependency (the runtime links SDL3, resolving these).
extern "C" {
typedef struct SDL_Window SDL_Window;
unsigned int SDL_GetWindowProperties(SDL_Window* window);
void* SDL_GetPointerProperty(unsigned int props, const char* name, void* default_value);
}

bool D3D12Swapchain::Init(u32 width, u32 height, bool vsync) {
  vsync_ = vsync;
  auto* sdl_window = static_cast<SDL_Window*>(device_.native_window());
  HWND hwnd = nullptr;
  if (sdl_window) {
    hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window),
                                                    "SDL.window.win32.hwnd", nullptr));
  }
  if (!hwnd) {
    REC_ERROR("d3d12: no HWND for the swapchain");
    return false;
  }

  IDXGIFactory4* factory = nullptr;
  if (FAILED(CreateDXGIFactory2(0, IID_IDXGIFactory4, reinterpret_cast<void**>(&factory)))) {
    REC_ERROR("d3d12: dxgi factory creation failed");
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
  desc.BufferCount = 3;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  IDXGISwapChain1* swapchain1 = nullptr;
  HRESULT hr = factory->CreateSwapChainForHwnd(device_.queue(), hwnd, &desc, nullptr, nullptr,
                                               &swapchain1);
  factory->Release();
  if (FAILED(hr)) {
    REC_ERROR("d3d12: swapchain creation failed");
    return false;
  }
  hr = swapchain1->QueryInterface(IID_IDXGISwapChain3, reinterpret_cast<void**>(&dxgi_));
  swapchain1->Release();
  if (FAILED(hr)) {
    REC_ERROR("d3d12: IDXGISwapChain3 unavailable");
    return false;
  }

  extent_ = {width, height};
  format_ = Format::kBGRA8Unorm;
  for (u32 i = 0; i < desc.BufferCount; ++i) {
    ID3D12Resource* buffer = nullptr;
    if (FAILED(dxgi_->GetBuffer(i, IID_ID3D12Resource, reinterpret_cast<void**>(&buffer)))) {
      REC_ERROR("d3d12: swapchain buffer {} unavailable", i);
      return false;
    }
    TextureRecord* record = device_.MakeTextureRecord(
        buffer, format_, extent_, 1, 1, false,
        kTextureUsageColorTarget | kTextureUsageSampled | kTextureUsageTransferSrc |
            kTextureUsageTransferDst);
    images_.push_back(device_.WrapTexture(record));
  }
  REC_INFO("swapchain: {}x{}, {} images, dxgi flip-model ({})", extent_.width, extent_.height,
           desc.BufferCount, vsync ? "vsync" : "immediate");
  return true;
}

D3D12Swapchain::~D3D12Swapchain() {
  device_.WaitIdle();
  for (GpuImage& image : images_) device_.DestroyImage(image);
  if (dxgi_) dxgi_->Release();
}

AcquireResult D3D12Swapchain::Acquire(u32 slot, u32* out_image_index) {
  (void)slot;  // pacing comes from the device's per-slot frame fences
  *out_image_index = dxgi_->GetCurrentBackBufferIndex();
  return AcquireResult::kOk;
}

PresentResult D3D12Swapchain::Present() {
  HRESULT hr = dxgi_->Present(vsync_ ? 1 : 0, 0);
  if (FAILED(hr)) return PresentResult::kOutOfDate;
  return PresentResult::kOk;
}

#else  // offscreen ring (vkd3d)

bool D3D12Swapchain::Init(u32 width, u32 height, bool vsync) {
  (void)vsync;  // nothing to pace against offscreen
  extent_ = {width, height};
  format_ = Format::kBGRA8Unorm;  // matches the vulkan backend's surface pick

  constexpr u32 kImageCount = 3;
  for (u32 i = 0; i < kImageCount; ++i) {
    GpuImage image = device_.CreateImage2D(
        format_, extent_,
        kTextureUsageColorTarget | kTextureUsageSampled | kTextureUsageTransferSrc |
            kTextureUsageTransferDst,
        1);
    if (!image) {
      REC_ERROR("d3d12: offscreen swapchain image creation failed");
      return false;
    }
    images_.push_back(image);
  }
  REC_INFO("swapchain: {}x{}, {} images, offscreen (no display path through vkd3d)",
           extent_.width, extent_.height, kImageCount);
  return true;
}

D3D12Swapchain::~D3D12Swapchain() {
  device_.WaitIdle();
  for (GpuImage& image : images_) device_.DestroyImage(image);
}

AcquireResult D3D12Swapchain::Acquire(u32 slot, u32* out_image_index) {
  (void)slot;  // frame pacing comes from the device's per-slot fences
  *out_image_index = static_cast<u32>(frame_counter_++ % images_.size());
  return AcquireResult::kOk;
}

PresentResult D3D12Swapchain::Present() { return PresentResult::kOk; }

#endif

}  // namespace rec::render::d3d12

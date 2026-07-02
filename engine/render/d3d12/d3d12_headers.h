#ifndef RECREATION_RENDER_D3D12_D3D12_HEADERS_H_
#define RECREATION_RENDER_D3D12_D3D12_HEADERS_H_

// D3D12 API include shim. On Windows the real SDK headers apply; everywhere
// else vkd3d's headers provide the identical API surface natively
// (vkd3d_windows.h supplies the COM/Windows base types, vkd3d_d3d12.h the API,
// vkd3d_utils.h D3D12CreateDevice/D3D12SerializeRootSignature and the event
// helpers). Include this before any engine header that uses std::min/max:
// vkd3d_windows.h defines min/max macros which are undone below.

#if defined(_WIN32)

#include <d3d12.h>
#include <dxgi1_6.h>

namespace rec::render::d3d12 {
inline HANDLE CreateFenceEvent() { return CreateEventA(nullptr, FALSE, FALSE, nullptr); }
inline void WaitFenceEvent(HANDLE event) { WaitForSingleObject(event, INFINITE); }
inline void DestroyFenceEvent(HANDLE event) { CloseHandle(event); }
}  // namespace rec::render::d3d12

#else  // vkd3d (Linux and friends)

// libvkd3d implements aggregate-returning methods (GetDesc,
// Get*DescriptorHandleForHeapStart, GetResourceAllocationInfo, ...) with the
// Windows ABI: the aggregate returns through an explicit out pointer. This
// macro makes the C++ headers declare that ABI (plus source-compatible inline
// wrappers); without it the SysV small-struct return convention reads a
// garbage out-pointer and crashes inside libvkd3d.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS

#include <vkd3d_utils.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace rec::render::d3d12 {
inline HANDLE CreateFenceEvent() { return vkd3d_create_event(); }
inline void WaitFenceEvent(HANDLE event) { vkd3d_wait_event(event, VKD3D_INFINITE); }
inline void DestroyFenceEvent(HANDLE event) { vkd3d_destroy_event(event); }
}  // namespace rec::render::d3d12

#endif

#endif  // RECREATION_RENDER_D3D12_D3D12_HEADERS_H_

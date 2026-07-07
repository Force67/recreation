#include "render/d3d12/d3d12_backend.h"

namespace rec::render::d3d12 {
namespace {

// Indexed by Format; kept in declaration order and size-checked so adding a
// format without extending the table fails at compile time (mirrors
// vk_convert.cc).
constexpr DXGI_FORMAT kFormatTable[] = {
    DXGI_FORMAT_UNKNOWN,              // kUnknown
    DXGI_FORMAT_R8_UNORM,             // kR8Unorm
    DXGI_FORMAT_R8_SNORM,             // kR8Snorm
    DXGI_FORMAT_R8_UINT,              // kR8Uint
    DXGI_FORMAT_R8G8_UNORM,           // kRG8Unorm
    DXGI_FORMAT_R8G8B8A8_UNORM,       // kRGBA8Unorm
    DXGI_FORMAT_R8G8B8A8_SNORM,       // kRGBA8Snorm
    DXGI_FORMAT_R8G8B8A8_UINT,        // kRGBA8Uint
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  // kRGBA8Srgb
    DXGI_FORMAT_B8G8R8A8_UNORM,       // kBGRA8Unorm
    DXGI_FORMAT_R16_UNORM,            // kR16Unorm
    DXGI_FORMAT_R16_SNORM,            // kR16Snorm
    DXGI_FORMAT_R16_UINT,             // kR16Uint
    DXGI_FORMAT_R16_FLOAT,            // kR16Float
    DXGI_FORMAT_R16G16_UNORM,         // kRG16Unorm
    DXGI_FORMAT_R16G16_SNORM,         // kRG16Snorm
    DXGI_FORMAT_R16G16_SINT,          // kRG16Sint
    DXGI_FORMAT_R16G16_FLOAT,         // kRG16Float
    DXGI_FORMAT_R16G16B16A16_UNORM,   // kRGBA16Unorm
    DXGI_FORMAT_R16G16B16A16_SNORM,   // kRGBA16Snorm
    DXGI_FORMAT_R16G16B16A16_FLOAT,   // kRGBA16Float
    DXGI_FORMAT_R32_UINT,             // kR32Uint
    DXGI_FORMAT_R32_FLOAT,            // kR32Float
    DXGI_FORMAT_R32G32_FLOAT,         // kRG32Float
    DXGI_FORMAT_R32G32B32_FLOAT,      // kRGB32Float (vertex fetch only)
    DXGI_FORMAT_R32G32B32A32_FLOAT,   // kRGBA32Float
    DXGI_FORMAT_R10G10B10A2_UNORM,    // kRGB10A2Unorm
    DXGI_FORMAT_R11G11B10_FLOAT,      // kRG11B10Float
    DXGI_FORMAT_R9G9B9E5_SHAREDEXP,   // kRGB9E5Float
    DXGI_FORMAT_D32_FLOAT,            // kD32Float
    DXGI_FORMAT_D16_UNORM,            // kD16Unorm
    DXGI_FORMAT_BC1_UNORM,            // kBC1RgbUnorm
    DXGI_FORMAT_BC1_UNORM_SRGB,       // kBC1RgbSrgb
    DXGI_FORMAT_BC2_UNORM,            // kBC2Unorm
    DXGI_FORMAT_BC2_UNORM_SRGB,       // kBC2Srgb
    DXGI_FORMAT_BC3_UNORM,            // kBC3Unorm
    DXGI_FORMAT_BC3_UNORM_SRGB,       // kBC3Srgb
    DXGI_FORMAT_BC4_UNORM,            // kBC4Unorm
    DXGI_FORMAT_BC5_UNORM,            // kBC5Unorm
    DXGI_FORMAT_BC7_UNORM,            // kBC7Unorm
    DXGI_FORMAT_BC7_UNORM_SRGB,       // kBC7Srgb
};
static_assert(sizeof(kFormatTable) / sizeof(kFormatTable[0]) == kFormatCount,
              "format table out of sync with rhi::Format");

}  // namespace

DXGI_FORMAT ToDxgiFormat(Format format) { return kFormatTable[static_cast<u32>(format)]; }

DXGI_FORMAT ToDxgiSrvFormat(Format format) {
  if (format == Format::kD32Float) return DXGI_FORMAT_R32_FLOAT;
  if (format == Format::kD16Unorm) return DXGI_FORMAT_R16_UNORM;
  return ToDxgiFormat(format);
}

// Legacy resource states (vkd3d 2.0 has no enhanced barriers; the mapping is
// still exact for everything the rhi expresses).
D3D12_RESOURCE_STATES ToResourceStates(ResourceState state) {
  switch (state) {
    case ResourceState::kUndefined: return D3D12_RESOURCE_STATE_COMMON;
    case ResourceState::kGeneral: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case ResourceState::kShaderReadCompute:
    case ResourceState::kShaderReadTaskMesh:
      return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case ResourceState::kShaderReadFragment:
      return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case ResourceState::kShaderReadAll:
      return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case ResourceState::kColorTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case ResourceState::kDepthTarget: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case ResourceState::kCopySrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ResourceState::kCopyDst: return D3D12_RESOURCE_STATE_COPY_DEST;
    // The offscreen ring has no display engine; PRESENT aliases COMMON in
    // D3D12 anyway, so the renderer's kPresent handoff maps cleanly.
    case ResourceState::kPresent: return D3D12_RESOURCE_STATE_COMMON;
    // Unreachable while caps.fragment_shading_rate stays false on this
    // backend (vkd3d 2.0 has no RSSetShadingRateImage), but the mapping is
    // exact for a runtime that grows it.
    case ResourceState::kShadingRate: return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
  }
  return D3D12_RESOURCE_STATE_COMMON;
}

D3D12_COMPARISON_FUNC ToCompareFunc(CompareOp op) {
  switch (op) {
    case CompareOp::kNever: return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::kLess: return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::kLessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::kEqual: return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::kGreater: return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::kGreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::kAlways: return D3D12_COMPARISON_FUNC_ALWAYS;
  }
  return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_TEXTURE_ADDRESS_MODE ToAddressMode(AddressMode mode) {
  switch (mode) {
    case AddressMode::kRepeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::kClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::kClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case AddressMode::kMirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
  }
  return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_FILTER ToFilter(const SamplerDesc& desc) {
  if (desc.max_anisotropy > 1.0f) {
    return desc.compare_enable ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
  }
  // D3D12_FILTER bit layout: mip = bit 0, mag = bit 2, min = bit 4,
  // comparison = bit 7.
  u32 filter = 0;
  if (desc.mip_filter == Filter::kLinear) filter |= 0x1;
  if (desc.mag_filter == Filter::kLinear) filter |= 0x4;
  if (desc.min_filter == Filter::kLinear) filter |= 0x10;
  if (desc.compare_enable) filter |= 0x80;
  return static_cast<D3D12_FILTER>(filter);
}

}  // namespace rec::render::d3d12

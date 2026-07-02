#ifndef RECREATION_RENDER_RHI_TYPES_H_
#define RECREATION_RENDER_RHI_TYPES_H_

#include "core/types.h"

// Backend-agnostic value types shared by the whole RHI. Nothing in this header
// (or any rhi/ header) may name a Vulkan, D3D12 or console type: the renderer
// is written against these, and each backend translates in its own directory
// (vulkan/, d3d12/, ...). Interop escape hatches live in per-backend headers
// (e.g. rhi/vulkan_interop.h) that only backend-aware modules include.

namespace rec::render {

// Texel formats actually used by the engine. One-to-one mappable to VkFormat
// and DXGI_FORMAT; extend as needed, keeping the mapping tables in each
// backend in sync (they static_assert on kFormatCount).
enum class Format : u8 {
  kUnknown = 0,
  // 8-bit
  kR8Unorm,
  kR8Snorm,
  kR8Uint,
  kRG8Unorm,
  kRGBA8Unorm,
  kRGBA8Snorm,
  kRGBA8Uint,
  kRGBA8Srgb,
  kBGRA8Unorm,
  // 16-bit
  kR16Unorm,
  kR16Snorm,
  kR16Uint,
  kR16Float,
  kRG16Unorm,
  kRG16Snorm,
  kRG16Sint,
  kRG16Float,
  kRGBA16Unorm,
  kRGBA16Snorm,
  kRGBA16Float,
  // 32-bit
  kR32Uint,
  kR32Float,
  kRG32Float,
  kRGB32Float,  // vertex fetch only
  kRGBA32Float,
  // packed
  kRGB10A2Unorm,
  kRG11B10Float,
  kRGB9E5Float,
  // depth
  kD32Float,
  // block compressed
  kBC1RgbUnorm,
  kBC1RgbSrgb,
  kBC2Unorm,
  kBC2Srgb,
  kBC3Unorm,
  kBC3Srgb,
  kBC4Unorm,
  kBC5Unorm,
  kBC7Unorm,
  kBC7Srgb,
  kCount,
};
constexpr u32 kFormatCount = static_cast<u32>(Format::kCount);

constexpr bool IsDepthFormat(Format format) { return format == Format::kD32Float; }

// Bytes per texel (per block for BC formats), for footprint estimates and
// tightly packed copies.
u32 FormatTexelBytes(Format format);

struct Extent2D {
  u32 width = 0;
  u32 height = 0;
  bool operator==(const Extent2D& o) const { return width == o.width && height == o.height; }
};

// Every distinct way the engine touches an image, from the render graph's
// usage table and the manual transitions in passes. Each backend maps a state
// to (layout, stage, access) / (D3D12 state, sync). States are deliberately
// coarse: precise-enough for correctness, portable across APIs.
enum class ResourceState : u8 {
  kUndefined = 0,     // contents discardable (fresh image / transient reuse)
  kGeneral,           // storage image read/write (UAV), any shader stage
  kShaderReadCompute,  // sampled in compute
  kShaderReadFragment, // sampled in fragment
  kShaderReadTaskMesh, // sampled in task/mesh (gpu cluster cull)
  kShaderReadAll,      // sampled anywhere (conservative import default)
  kColorTarget,
  kDepthTarget,
  kCopySrc,
  kCopyDst,
  kPresent,
};

// Coarse execution+memory scopes for global (non-image) barriers. Covers every
// buffer hazard the passes express today; extend rather than exposing raw
// stage/access masks.
enum class BarrierScope : u8 {
  kNone = 0,
  kAllCommands,
  kComputeWrite,      // compute shader storage writes
  kComputeRead,       // compute shader reads (storage/sampled/uniform)
  kGraphicsRead,      // any graphics-stage read (vertex/index/uniform/sampled)
  kIndirectArgs,      // indirect draw/dispatch argument fetch
  kTransferWrite,
  kTransferRead,
  kAccelBuildWrite,   // acceleration structure build output
  kAccelRead,         // ray query consumption of a built TLAS/BLAS
  kHostRead,          // cpu readback after fence wait
};

enum class LoadOp : u8 { kLoad, kClear, kDontCare };
enum class StoreOp : u8 { kStore, kDontCare };

enum class CompareOp : u8 {
  kNever,
  kLess,
  kLessEqual,
  kEqual,
  kGreater,
  kGreaterEqual,
  kAlways,
};

enum class Filter : u8 { kNearest, kLinear };
enum class AddressMode : u8 { kRepeat, kClampToEdge, kClampToBorder, kMirroredRepeat };
enum class BorderColor : u8 { kTransparentBlack, kOpaqueBlack, kOpaqueWhite };

struct SamplerDesc {
  Filter min_filter = Filter::kLinear;
  Filter mag_filter = Filter::kLinear;
  Filter mip_filter = Filter::kLinear;
  AddressMode address_u = AddressMode::kRepeat;
  AddressMode address_v = AddressMode::kRepeat;
  AddressMode address_w = AddressMode::kRepeat;
  f32 max_anisotropy = 1.0f;  // <= 1 disables
  f32 min_lod = 0.0f;
  f32 max_lod = 1000.0f;
  BorderColor border = BorderColor::kOpaqueBlack;
  bool compare_enable = false;  // shadow pcf samplers
  CompareOp compare_op = CompareOp::kLessEqual;

  bool operator==(const SamplerDesc&) const = default;
};

enum class PrimitiveTopology : u8 { kTriangleList, kTriangleStrip, kLineList, kPointList };
enum class CullMode : u8 { kNone, kBack, kFront };
enum class FrontFace : u8 { kCounterClockwise, kClockwise };
enum class PolygonMode : u8 { kFill, kLine };
enum class IndexType : u8 { kUint16, kUint32 };

// The fixed set of blend configurations the passes use. A preset enum keeps
// pipeline descs terse and trivially portable; add presets before reaching
// for a full blend-factor matrix.
enum class BlendMode : u8 {
  kOpaque,          // blending off
  kAlpha,           // srcAlpha, oneMinusSrcAlpha
  kPremultiplied,   // one, oneMinusSrcAlpha
  kAdditive,        // one, one
  kMultiply,        // zero, srcColor
  kWboitAccum,      // one, one (color+alpha) - weighted-blended OIT accumulation
  kWboitReveal,     // zero, oneMinusSrcColor - wboit revealage
};

// Buffer usages as the engine needs them; each backend widens to its API's
// flags. Combining flags is fine (e.g. kStorage | kIndirect for cull output).
enum BufferUsage : u32 {
  kBufferUsageNone = 0,
  kBufferUsageVertex = 1u << 0,
  kBufferUsageIndex = 1u << 1,
  kBufferUsageUniform = 1u << 2,
  kBufferUsageStorage = 1u << 3,
  kBufferUsageIndirect = 1u << 4,
  kBufferUsageTransferSrc = 1u << 5,
  kBufferUsageTransferDst = 1u << 6,
  kBufferUsageDeviceAddress = 1u << 7,  // shader access via u64 address
  kBufferUsageAccelBuildInput = 1u << 8,   // BLAS build geometry source
  kBufferUsageAccelStorage = 1u << 9,      // acceleration structure backing
  kBufferUsageAccelScratch = 1u << 10,     // acceleration structure scratch
};
using BufferUsageFlags = u32;

enum TextureUsage : u32 {
  kTextureUsageNone = 0,
  kTextureUsageSampled = 1u << 0,
  kTextureUsageStorage = 1u << 1,
  kTextureUsageColorTarget = 1u << 2,
  kTextureUsageDepthTarget = 1u << 3,
  kTextureUsageTransferSrc = 1u << 4,
  kTextureUsageTransferDst = 1u << 5,
};
using TextureUsageFlags = u32;

enum ShaderStage : u32 {
  kShaderStageNone = 0,
  kShaderStageVertex = 1u << 0,
  kShaderStageFragment = 1u << 1,
  kShaderStageCompute = 1u << 2,
  kShaderStageTask = 1u << 3,
  kShaderStageMesh = 1u << 4,
};
using ShaderStageFlags = u32;

// Bytecode container. The build always embeds SPIR-V; when the d3d12 backend
// is compiled in (RECREATION_RHI_D3D12) every shader also carries a DXIL
// sidecar and each backend picks its format. Pass code never looks inside.
enum class ShaderFormat : u8 { kSpirv, kDxil };

struct ShaderBlob {
  const unsigned char* data = nullptr;  // SPIR-V words (byte packed)
  size_t size = 0;
  ShaderFormat format = ShaderFormat::kSpirv;
  // DXIL sidecar; null when the build has no d3d12 backend. The d3d12 device
  // fails pipeline creation gracefully on an empty sidecar.
  const unsigned char* dxil_data = nullptr;
  size_t dxil_size = 0;

  bool valid() const { return data != nullptr && size > 0; }
  bool dxil_valid() const { return dxil_data != nullptr && dxil_size > 0; }
};

// Wraps a build-embedded shader byte array (build/generated/shaders/*.h) as a
// blob. With RECREATION_RHI_D3D12 the embed step emits a k_<sym>_dxil sidecar
// array alongside the SPIR-V and both ride in the same blob.
#if defined(RECREATION_RHI_D3D12)
#define REC_SHADER(sym)                                                       \
  (::rec::render::ShaderBlob{sym, sizeof(sym),                                \
                             ::rec::render::ShaderFormat::kSpirv, sym##_dxil, \
                             sizeof(sym##_dxil)})
#else
#define REC_SHADER(sym) \
  (::rec::render::ShaderBlob{sym, sizeof(sym), ::rec::render::ShaderFormat::kSpirv})
#endif

// Type-safe opaque handles. The value is a backend-owned record pointer (or
// packed handle); 0 is always "null". Handles are plain values: copying never
// transfers ownership, destruction goes through the Device.
template <typename Tag>
struct RhiHandle {
  u64 value = 0;
  explicit operator bool() const { return value != 0; }
  bool operator==(const RhiHandle&) const = default;
};

using TextureView = RhiHandle<struct TextureViewTag>;
using SamplerHandle = RhiHandle<struct SamplerTag>;
using PipelineHandle = RhiHandle<struct PipelineTag>;
using BindingLayoutHandle = RhiHandle<struct BindingLayoutTag>;
using BindingSetHandle = RhiHandle<struct BindingSetTag>;
using AccelStructHandle = RhiHandle<struct AccelStructTag>;
using TimestampPoolHandle = RhiHandle<struct TimestampPoolTag>;

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_TYPES_H_

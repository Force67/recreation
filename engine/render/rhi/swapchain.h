#ifndef RECREATION_RENDER_RHI_SWAPCHAIN_H_
#define RECREATION_RENDER_RHI_SWAPCHAIN_H_

#include "core/types.h"
#include "render/rhi/resources.h"
#include "render/rhi/types.h"

namespace rec::render {

// Transfer function + primaries of the presented image. The engine renders
// linear Rec.709 internally; the tonemap pass encodes for whichever of these
// the swapchain negotiated.
enum class ColorSpace : u8 {
  kSrgbNonlinear,  // 8/10-bit SDR, sRGB OETF (the default everywhere)
  kHdr10Pq,        // 10-bit Rec.2020 + ST2084 PQ
  kScRgbLinear,    // fp16 linear, sRGB primaries, 1.0 = 80 nits
};

enum class AcquireResult : u8 {
  kOk,
  kSuboptimal,  // usable this frame; recreate when convenient
  kOutOfDate,   // recreate before rendering
  kFailed,
};

// Presentation images. Created via Device::CreateSwapchain; presentation goes
// through Device::SubmitFrame (which owns the acquire/present sync for the
// frame slot passed to Acquire).
class Swapchain {
 public:
  virtual ~Swapchain() = default;

  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;

  // Acquires the next image using frame slot `slot`'s sync primitives; the
  // following Device::SubmitFrame with the same slot waits on it.
  virtual AcquireResult Acquire(u32 slot, u32* out_image_index) = 0;

  virtual Format format() const = 0;
  virtual Extent2D extent() const = 0;
  virtual u32 image_count() const = 0;
  virtual const GpuImage& image(u32 index) const = 0;
  // True when the backbuffer can be sampled (e.g. for UI backdrop blur).
  virtual bool can_sample() const = 0;
  // What the tonemap pass must encode; kSrgbNonlinear unless an HDR swapchain
  // was requested AND the surface offers one.
  virtual ColorSpace color_space() const { return ColorSpace::kSrgbNonlinear; }

 protected:
  Swapchain() = default;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_SWAPCHAIN_H_

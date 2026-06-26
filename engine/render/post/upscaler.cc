#include "render/post/upscaler.h"

#include "core/log.h"
#include "render/rhi/device.h"

namespace rec::render {

#if defined(RECREATION_HAS_FSR3)
std::unique_ptr<Upscaler> CreateFsr3Upscaler(const UpscalerDesc& desc, Device& device);
#endif
#if defined(RECREATION_HAS_DLSS)
std::unique_ptr<Upscaler> CreateDlssUpscaler(const UpscalerDesc& desc, Device& device);
#endif

// SDK backed implementations (FSR3, DLSS, XeSS) plug in here behind build
// options. Anything not compiled in falls back to TAA.
std::unique_ptr<Upscaler> CreateUpscaler(const UpscalerDesc& desc, Device& device) {
  switch (desc.kind) {
    case UpscalerKind::kFsr3:
#if defined(RECREATION_HAS_FSR3)
      if (!device.is_stub()) return CreateFsr3Upscaler(desc, device);
#endif
      REC_WARN("upscaler backend not compiled in");
      return nullptr;
    case UpscalerKind::kDlss:
#if defined(RECREATION_HAS_DLSS)
      if (!device.is_stub()) return CreateDlssUpscaler(desc, device);
#endif
      REC_WARN("upscaler backend not compiled in");
      return nullptr;
    case UpscalerKind::kXess:
      REC_WARN("upscaler backend not compiled in");
      return nullptr;
    case UpscalerKind::kNone:
      return nullptr;
  }
  return nullptr;
}

}  // namespace rec::render

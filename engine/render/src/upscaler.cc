#include "recreation/render/upscaler.h"

#include "recreation/core/log.h"
#include "recreation/render/rhi/device.h"

namespace rec::render {

// SDK backed implementations (FSR3, DLSS, XeSS) plug in here behind build
// options once the RHI exposes what they need. Until then every request
// falls back to TAA.
std::unique_ptr<Upscaler> CreateUpscaler(const UpscalerDesc& desc, Device& device) {
  switch (desc.kind) {
    case UpscalerKind::kFsr3:
    case UpscalerKind::kDlss:
    case UpscalerKind::kXess:
      REC_WARN("upscaler backend not compiled in");
      return nullptr;
    case UpscalerKind::kNone:
      return nullptr;
  }
  return nullptr;
}

}  // namespace rec::render

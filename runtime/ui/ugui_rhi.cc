// Headless no-op ugui::RHI for the recreation engine. Compiled into the
// ultragui static library in place of the bundled Vulkan RHI
// (ULTRAGUI_RHI_SOURCE). In draw-data mode the host engine owns the GPU and
// renders the draw list itself, so the bundled RHI is never initialized; this
// stub keeps the library free of any Vulkan (and the volk/loader conflict it
// would otherwise cause against the engine's own renderer).

#include <ugui/rhi/rhi.h>

namespace ugui {

struct RHI::Impl {};

RHI::RHI() : impl_(nullptr) {}
RHI::~RHI() {}

bool RHI::Init(const RHIConfig&) { return false; }
void RHI::Shutdown() {}

bool RHI::BeginFrame(Color) { return false; }
void RHI::EndFrame() {}

void RHI::SetScissor(Rect) {}
void RHI::ResetScissor() {}

void RHI::DrawTriangles(const Vertex2D*, u32, const u32*, u32, RHITextureHandle) {}
void RHI::DrawTextTriangles(const Vertex2D*, u32, const u32*, u32, RHITextureHandle) {}

RHITextureHandle RHI::CreateTexture(u32, u32, RHIFormat, const void*, RHIFilter) {
  return kInvalidTexture;
}
void RHI::UpdateTexture(RHITextureHandle, const void*) {}
void RHI::DestroyTexture(RHITextureHandle) {}

bool RHI::AcquireFrame() { return false; }

RHITextureHandle RHI::CreateRenderTarget(u32, u32) { return kInvalidTexture; }
void RHI::DestroyRenderTarget(RHITextureHandle) {}
bool RHI::BeginOffscreen(RHITextureHandle, Color) { return false; }
void RHI::EndOffscreen(RHITextureHandle) {}
void RHI::ConvertVideoFrame(RHITextureHandle, RHITextureHandle, RHITextureHandle,
                            RHITextureHandle) {}

Vec2 RHI::display_size() const { return {0.0f, 0.0f}; }
f32 RHI::dpi_scale() const { return 1.0f; }

}  // namespace ugui

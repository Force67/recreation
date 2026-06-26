#ifndef RECREATION_RENDER_GAUSSIAN_H_
#define RECREATION_RENDER_GAUSSIAN_H_

#include <string>

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class Device;

// One 3D gaussian, filled by the engine and handed to the renderer through
// FrameView. Matches the Gaussian struct in gsplat.vs (std430).
struct GaussianInstance {
  f32 position[3] = {0, 0, 0};
  f32 opacity = 1.0f;
  f32 scale[3] = {0.1f, 0.1f, 0.1f};
  f32 pad0 = 0;
  f32 rotation[4] = {0, 0, 0, 1};  // quaternion xyzw
  f32 color[3] = {1, 1, 1};
  f32 pad1 = 0;
};

// 3D gaussian splatting: a non-triangle primitive path. Projects each gaussian's
// 3D covariance to a 2D screen ellipse (EWA splatting), evaluated through the
// conic in the pixel shader, alpha blended back-to-front. The renderer sorts the
// live set by view depth each frame and uploads it to a per-frame buffer; the
// architecture treats splats as first-class drawables alongside triangles.
class GaussianSplat {
 public:
  bool Initialize(Device& device, VkFormat color_format);
  void Destroy(Device& device);

  struct Frame {
    Mat4 view;       // world -> view
    f32 proj_x = 1;  // P[0][0]
    f32 proj_y = -1;  // P[1][1]
    f32 near_plane = 0.1f;
    f32 screen_x = 0;
    f32 screen_y = 0;
  };

  // Sorts the gaussians back-to-front by view depth, uploads them and adds the
  // splat pass blending into color. No-op when empty.
  void AddToGraph(RenderGraph& graph, ResourceHandle color,
                  const base::Vector<GaussianInstance>& gaussians, const Frame& frame,
                  u32 frame_slot);

 private:
  static constexpr u32 kFramesInFlight = 2;
  static constexpr u32 kMaxGaussians = 1u << 18;  // 262144

  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  GpuBuffer buffers_[kFramesInFlight];
};

// Loads an INRIA 3D Gaussian Splatting .ply (ascii or binary little-endian) into
// GaussianInstances, applying the standard activations: sigmoid opacity, exp
// scale, sh band-0 dc color, normalized wxyz->xyzw rotation. Returns false on a
// read/parse error. Caps at the renderer's gaussian budget (logs if truncated).
bool LoadGaussianPly(const std::string& path, base::Vector<GaussianInstance>* out);

}  // namespace rec::render

#endif  // RECREATION_RENDER_GAUSSIAN_H_

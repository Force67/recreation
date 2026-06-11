#ifndef RECREATION_RENDER_RENDERER_H_
#define RECREATION_RENDER_RENDERER_H_

#include <memory>
#include <unordered_map>
#include <vector>

#include "asset/mesh.h"
#include "core/math.h"
#include "core/window.h"
#include "render/antialiasing.h"
#include "render/mesh_pipeline.h"
#include "render/post.h"
#include "render/raytracing.h"
#include "render/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"
#include "render/upscaler.h"

namespace rec::render {

struct RendererDesc {
  bool enable_validation = false;
  AntiAliasingMode aa_mode = AntiAliasingMode::kTaa;
  UpscalerKind upscaler = UpscalerKind::kNone;
  RayTracingSettings raytracing;
  bool enable_raytracing = true;
};

struct CameraPose {
  Vec3 eye{0, 0, 3};
  Vec3 target{};
  f32 fov_y = 1.0472f;  // 60 degrees
};

// What the simulation hands the renderer each frame. The engine extracts
// this from the ECS, keeping the renderer free of gameplay types. The
// previous frame's transform feeds motion vectors; for static or newly
// spawned objects it equals transform.
struct DrawItem {
  u64 mesh = 0;  // AssetId hash of an uploaded mesh
  Mat4 transform = Mat4::Identity();
  Mat4 prev_transform = Mat4::Identity();
};

struct FrameView {
  CameraPose camera;
  std::vector<DrawItem> draws;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  bool Initialize(const RendererDesc& desc, Window& window);
  void RenderFrame(const FrameView& view);
  void Shutdown();

  // Makes a mesh drawable, keyed by its asset id. No-op without a device.
  bool UploadMesh(const asset::Mesh& mesh);

  // Switching AA or upscaler at runtime resets temporal history.
  void SetAntiAliasing(AntiAliasingMode mode);
  void SetUpscaler(UpscalerKind kind);

  const DeviceCaps* caps() const;

 private:
  static constexpr u32 kFramesInFlight = 2;
  static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
  static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

  struct FrameResources {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    GpuBuffer globals;  // host visible FrameGlobals
  };

  bool CreateFrameResources();
  void DestroyFrameResources();
  bool CreateRenderFinishedSemaphores();
  void DestroyRenderFinishedSemaphores();
  void RecreateSwapchain();
  void UpdateRenderResolution();
  void BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view);

  RendererDesc desc_;
  Window* window_ = nullptr;
  std::unique_ptr<Device> device_;
  std::unique_ptr<Swapchain> swapchain_;
  std::unique_ptr<TransientPool> transient_pool_;
  std::unique_ptr<MeshPipeline> mesh_pipeline_;
  std::unique_ptr<PostPass> post_;
  std::unordered_map<u64, GpuMesh> meshes_;
  FrameResources frames_[kFramesInFlight];
  // One per swapchain image: a present may still wait on the semaphore
  // until its image is reacquired, so frame slots cannot own these.
  std::vector<VkSemaphore> render_finished_;
  std::unique_ptr<Upscaler> upscaler_;
  std::unique_ptr<RayTracingContext> raytracing_;
  RenderGraph graph_;
  TaaPass taa_;
  Mat4 prev_view_proj_ = Mat4::Identity();
  bool has_prev_frame_ = false;
  bool rt_shadows_ = false;
  u32 frame_index_ = 0;
  u32 render_width_ = 0;
  u32 render_height_ = 0;
  u32 output_width_ = 0;
  u32 output_height_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDERER_H_

#ifndef RECREATION_RENDER_RENDERER_H_
#define RECREATION_RENDER_RENDERER_H_

#include <functional>
#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "core/math.h"
#include "core/window.h"
#include "render/ambient_occlusion.h"
#include "render/antialiasing.h"
#include "render/bindless.h"
#include "render/bloom.h"
#include "render/ddgi.h"
#include "render/exposure.h"
#include "render/environment.h"
#include "render/material_system.h"
#include "render/mesh_pipeline.h"
#include "render/post.h"
#include "render/raytracing.h"
#include "render/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"
#include "render/settings.h"
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
  f32 frame_delta_seconds = 1.0f / 60.0f;  // upscalers want real frame time
  base::Vector<DrawItem> draws;
  // Recorded inside the final ui pass with the backbuffer bound as the
  // color attachment. The debug ui renders ImGui draw data here.
  std::function<void(VkCommandBuffer)> ui_draw;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  bool Initialize(const RendererDesc& desc, Window& window);
  void RenderFrame(const FrameView& view);
  void Shutdown();
  void WaitIdle();

  // Makes a mesh drawable, keyed by its asset id. Materials referenced by
  // submeshes should be uploaded first. No-op without a device.
  bool UploadMesh(const asset::Mesh& mesh);
  bool UploadTexture(const asset::Texture& texture);
  bool UploadMaterial(const asset::Material& material);

  // Live tunables. Mutate freely; RenderFrame diffs against the applied
  // state and reconfigures, including full upscaler swaps.
  RenderSettings& settings() { return settings_; }

  const DeviceCaps* caps() const;
  Device* device() { return device_.get(); }
  VkFormat swapchain_format() const;
  u32 swapchain_image_count() const;
  u32 render_width() const { return render_width_; }
  u32 render_height() const { return render_height_; }
  u32 output_width() const { return output_width_; }
  u32 output_height() const { return output_height_; }
  bool upscaler_active() const { return upscaler_ != nullptr; }
  u32 mesh_count() const { return static_cast<u32>(meshes_.size()); }
  const MaterialSystem* materials() const { return material_system_.get(); }

 private:
  static constexpr u32 kFramesInFlight = 2;
  static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
  static constexpr VkFormat kNormalFormat = VK_FORMAT_R16G16_SFLOAT;
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
  void ApplySettings();
  bool CreateUpscalerForSettings();
  void BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view);

  RendererDesc desc_;
  RenderSettings settings_;
  Window* window_ = nullptr;
  std::unique_ptr<Device> device_;
  std::unique_ptr<Swapchain> swapchain_;
  std::unique_ptr<TransientPool> transient_pool_;
  std::unique_ptr<BindlessRegistry> bindless_;
  std::unique_ptr<MaterialSystem> material_system_;
  std::unique_ptr<EnvironmentSystem> environment_;
  std::unique_ptr<DdgiSystem> ddgi_;
  std::unique_ptr<MeshPipeline> mesh_pipeline_;
  std::unique_ptr<PostPass> post_;
  base::UnorderedMap<u64, GpuMesh> meshes_;
  FrameResources frames_[kFramesInFlight];
  // One per swapchain image: a present may still wait on the semaphore
  // until its image is reacquired, so frame slots cannot own these.
  base::Vector<VkSemaphore> render_finished_;
  std::unique_ptr<Upscaler> upscaler_;
  std::unique_ptr<RayTracingContext> raytracing_;
  RenderGraph graph_;
  TaaPass taa_;
  RtaoPass rtao_;
  BloomPass bloom_;
  ExposurePass exposure_;

  // Settings already in effect, diffed against settings_ each frame.
  UpscalerKind applied_upscaler_ = UpscalerKind::kNone;
  UpscalerQuality applied_quality_ = UpscalerQuality::kQuality;
  AntiAliasingMode applied_aa_ = AntiAliasingMode::kTaa;
  bool applied_vsync_ = false;
  // Sun state baked into the environment maps; differing means regenerate.
  Vec3 applied_sun_direction_{};
  f32 applied_sun_intensity_ = -1;
  Vec3 applied_sun_color_{};
  bool environment_dirty_ = true;

  Mat4 prev_view_proj_ = Mat4::Identity();
  bool has_prev_frame_ = false;
  bool rt_available_ = false;
  u32 frame_index_ = 0;
  u32 render_width_ = 0;
  u32 render_height_ = 0;
  u32 output_width_ = 0;
  u32 output_height_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDERER_H_

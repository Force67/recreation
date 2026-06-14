#ifndef RECREATION_RENDER_RENDERER_H_
#define RECREATION_RENDER_RENDERER_H_

#include <functional>
#include <memory>
#include <string>

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
#include "render/denoiser_nrd.h"
#include "render/exposure.h"
#include "render/environment.h"
#include "render/gaussian.h"
#include "render/gpu_cull.h"
#include "render/gpu_profiler.h"
#include "render/path_tracer.h"
#include "render/volumetric_fog.h"
#include "render/material_system.h"
#include "render/mesh_pipeline.h"
#include "render/overdraw.h"
#include "render/particles.h"
#include "render/post.h"
#include "render/raytracing.h"
#include "render/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"
#include "render/settings.h"
#include "render/shadow.h"
#include "render/ssao.h"
#include "render/ssr.h"
#include "render/upscaler.h"
#include "render/water.h"

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
  // Index of this mesh's first bone in FrameView::bone_matrices, -1 = static.
  // Only meaningful for skinned meshes.
  i32 skin_offset = -1;
};

struct FrameView {
  CameraPose camera;
  f32 frame_delta_seconds = 1.0f / 60.0f;  // upscalers want real frame time
  base::Vector<DrawItem> draws;
  // Bone palette for every skinned draw this frame, concatenated; each skinned
  // DrawItem indexes its run by skin_offset. Column-major model-space matrices.
  base::Vector<Mat4> bone_matrices;
  // Live billboard particles for this frame (engine-simulated). Drawn lit and
  // soft-faded over the resolved scene before reconstruction.
  base::Vector<ParticleInstance> particles;
  // 3D gaussian splats: non-triangle primitives, projected and alpha blended
  // over the resolved scene.
  base::Vector<GaussianInstance> gaussians;
  // Recorded inside the final ui pass with the backbuffer bound as the
  // color attachment. hud_draw (the libultragui HUD/menu) records first, then
  // ui_draw (the debug ImGui overlay) on top.
  std::function<void(VkCommandBuffer)> hud_draw;
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

  // Saves the next presented frame as png. Also armed by the
  // REC_SCREENSHOT env var ("path.png:seconds") for headless captures.
  void CaptureScreenshot(const std::string& path);

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

  // Per-pass GPU timings from the last resolved frame, for the debug overlay.
  const base::Vector<GpuProfiler::PassTiming>& pass_timings() const {
    return profiler_.results();
  }
  f32 gpu_frame_ms() const { return profiler_.total_ms(); }
  u32 path_trace_samples() const { return path_tracer_.accumulated_samples(); }

  // Last compiled frame graph, for the debug inspector (passes, transient
  // resources, barrier and memory totals).
  const RenderGraph::Stats& graph_stats() const { return graph_.stats(); }

  // Opaque indirect draw counts for the debug overlay: total submitted vs the
  // count that survived gpu frustum culling (one frame stale).
  u32 draws_total() const { return cull_total_commands_; }
  u32 draws_visible() const { return cull_visible_; }

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
    GpuBuffer bone_palette;  // host visible skinning matrices, read by device address
    VkDeviceAddress bone_palette_address = 0;
  };
  // Max bones across all skinned draws in one frame.
  static constexpr u32 kMaxFrameBones = 8192;

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
  std::unique_ptr<WaterPass> water_;
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
  SsaoPass ssao_;
  SsrPass ssr_;
  ShadowPass shadow_;
#if defined(RECREATION_HAS_NRD)
  NrdDenoiser nrd_;
#endif
  BloomPass bloom_;
  ExposurePass exposure_;
  GpuProfiler profiler_;
  PathTracer path_tracer_;
  VolumetricFog volumetric_fog_;
  ParticleSystem particles_;
  GaussianSplat gaussians_;
  OverdrawPass overdraw_;
  GpuCull gpu_cull_;
  Mat4 pt_prev_view_proj_ = Mat4::Identity();
  f32 pt_prev_sig_ = 0;  // lighting signature; change resets accumulation
  bool pt_was_active_ = false;

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

  void WriteScreenshot(u32 image_index);

  std::string screenshot_path_;
  f64 screenshot_at_ = -1;  // seconds; <0 means immediately when armed
  Mat4 prev_view_proj_ = Mat4::Identity();
  Mat4 prev_view_ = Mat4::Identity();
  Mat4 prev_proj_ = Mat4::Identity();
  f32 prev_jitter_[2] = {0, 0};
  f64 time_seconds_ = 0;
  bool has_prev_frame_ = false;
  bool rt_available_ = false;
  u32 frame_index_ = 0;
  u32 cull_total_commands_ = 0;  // opaque indirect draws this frame
  u32 cull_visible_ = 0;         // survivors from the last completed cull (fence-safe)
  u32 render_width_ = 0;
  u32 render_height_ = 0;
  u32 output_width_ = 0;
  u32 output_height_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDERER_H_

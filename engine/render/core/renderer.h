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
#include "render/screenspace/ambient_occlusion.h"
#include "render/post/antialiasing.h"
#include "render/core/bindless.h"
#include "render/post/bloom.h"
#include "render/gi/ddgi.h"
#include "render/gi/denoiser_nrd.h"
#include "render/post/exposure.h"
#include "render/atmosphere/environment.h"
#include "render/geometry/gaussian.h"
#include "render/pipeline/gpu_cull.h"
#include "render/util/gpu_profiler.h"
#include "render/pipeline/meshlet.h"
#include "render/atmosphere/aerial_perspective.h"
#include "render/atmosphere/clouds.h"
#include "render/gi/path_tracer.h"
#include "render/gi/recon_path_tracer.h"
#include "render/atmosphere/precipitation.h"
#include "render/atmosphere/surface_weather.h"
#include "render/atmosphere/volumetric_fog.h"
#include "render/pipeline/material_system.h"
#include "render/pipeline/mesh_pipeline.h"
#include "render/post/overdraw.h"
#include "render/geometry/particles.h"
#include "render/post/post.h"
#include "render/post/ui_blur.h"
#include "render/gi/raytracing.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"
#include "render/core/settings.h"
#include "render/geometry/fur.h"
#include "render/gi/shadow.h"
#include "render/gi/shadow_trace.h"
#include "render/geometry/wboit.h"
#include "render/screenspace/ssao.h"
#include "render/screenspace/ssgi.h"
#include "render/screenspace/ssr.h"
#include "render/post/upscaler.h"
#include "render/geometry/water.h"

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
  // Packed rgb8 tint (0xRRGGBB) modulating this draw's albedo, 0 = untinted.
  // Colours skinned actors by faction/team (e.g. red vs blue armies).
  u32 tint = 0;
};

struct FrameView {
  CameraPose camera;
  f32 frame_delta_seconds = 1.0f / 60.0f;  // upscalers want real frame time
  base::Vector<DrawItem> draws;
  // Dynamic omni lights this frame, accumulated in the forward lighting pass.
  base::Vector<PointLight> lights;
  // Bone palette for every skinned draw this frame, concatenated; each skinned
  // DrawItem indexes its run by skin_offset. Column-major model-space matrices.
  base::Vector<Mat4> bone_matrices;
  // Live billboard particles for this frame (engine-simulated). Drawn lit and
  // soft-faded over the resolved scene before reconstruction.
  base::Vector<ParticleInstance> particles;
  // gpu-simulated particle fountain: when count > 0, the renderer steps the
  // simulation on the gpu (compute) and draws it, instead of the cpu particles.
  u32 gpu_particle_count = 0;
  Vec3 gpu_particle_emitter{};
  // shell-fur ball: when enabled, the fur pass draws a fuzzy sphere here.
  bool fur_ball = false;
  Vec3 fur_position{};
  // order-independent transparency instances (wboit demo).
  base::Vector<WboitInstance> oit;
  // 3D gaussian splats: non-triangle primitives, projected and alpha blended
  // over the resolved scene.
  base::Vector<GaussianInstance> gaussians;
  // Recorded inside the final ui pass with the backbuffer bound as the
  // color attachment. hud_draw (the libultragui HUD/menu) records first, then
  // ui_draw (the debug ImGui overlay) on top.
  std::function<void(VkCommandBuffer)> hud_draw;
  std::function<void(VkCommandBuffer)> ui_draw;

  // Backdrop blur: when a frosted (backdrop-blur) widget is present, the UI sets
  // needs_blur so the renderer captures + blurs the backbuffer before the ui
  // pass and writes the result here for hud_draw to bind. blur_source/sampler
  // are filled by the renderer inside the ui pass, just before hud_draw runs.
  bool needs_blur = false;
  // Filled by the renderer during the (const) frame record, hence mutable.
  mutable VkImageView blur_source = VK_NULL_HANDLE;
  mutable VkSampler blur_sampler = VK_NULL_HANDLE;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  bool Initialize(const RendererDesc& desc, Window& window);
  void RenderFrame(const FrameView& view);
  void Shutdown();
  void WaitIdle();

  // Android lifecycle: the surface is lost when the activity's window goes away
  // (background) and rebound when it returns. DestroySurface tears down the
  // swapchain + surface; RecreateSurface rebinds to the current window and
  // rebuilds the swapchain. RenderFrame is a no-op while the surface is gone.
  void DestroySurface();
  void RecreateSurface();
  bool has_surface() const { return swapchain_ != nullptr; }

  // Saves the next presented frame as png. Also armed by the
  // REC_SCREENSHOT env var ("path.png:seconds") for headless captures.
  void CaptureScreenshot(const std::string& path);

  // Makes a mesh drawable, keyed by its asset id. Materials referenced by
  // submeshes should be uploaded first. No-op without a device. id_salt
  // namespaces the mesh/BLAS key so two content domains with colliding asset
  // paths (Skyrim and Fallout 4 both ship "meshes/...") do not overwrite each
  // other; entities must carry the salted id in their Renderable. Zero (the
  // primary game) leaves the key unchanged.
  bool UploadMesh(const asset::Mesh& mesh, u64 id_salt = 0);
  // Same per-domain salt as UploadMesh; it must match so a mesh's submesh
  // material references resolve to this domain's materials/textures.
  bool UploadTexture(const asset::Texture& texture, u64 id_salt = 0);
  bool UploadMaterial(const asset::Material& material, u64 id_salt = 0);
  // Builds + uploads a mesh for the mesh-shader meshlet path (the --demo meshlet
  // scene draws it instead of the normal raster geometry).
  void UploadMeshletMesh(const asset::Mesh& mesh);

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

  // Mesh-shader meshlet counts (0 total when no meshlet mesh is loaded): total
  // clusters vs the count that survived gpu frustum + cone cluster culling.
  u32 meshlets_total() const { return meshlet_.meshlet_count(); }
  u32 meshlets_visible() const { return meshlet_visible_; }

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
    GpuBuffer lights;  // host visible PointLight array
  };
  // Max bones across all skinned draws in one frame.
  static constexpr u32 kMaxFrameBones = 8192;
  static constexpr u32 kMaxFrameLights = 256;

  bool CreateFrameResources();
  void DestroyFrameResources();
  bool CreateRenderFinishedSemaphores();
  void DestroyRenderFinishedSemaphores();
  void RecreateSwapchain();
  void UpdateRenderResolution();
  void ApplySettings();
  bool CreateUpscalerForSettings();
  void BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view);
  // Builds the blas + bindless geometry for grass-like (no_rt) meshes uploaded
  // while path tracing was off, so enabling it later still gets the alpha-tested
  // foliage into the tlas. Idempotent (skips already-built meshes).
  void EnsureRayTracingGeometry();

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
  std::unique_ptr<UiBlurPass> ui_blur_;  // frosted-glass backdrop blur for the UI
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
  SsgiPass ssgi_;
  ShadowPass shadow_;
#if defined(RECREATION_HAS_NRD)
  NrdDenoiser nrd_;
  ShadowTracePass shadow_trace_;
#endif
  BloomPass bloom_;
  ExposurePass exposure_;
  GpuProfiler profiler_;
  PathTracer path_tracer_;
  ReconPathTracer recon_path_tracer_;
  VolumetricFog volumetric_fog_;
  AerialPerspective aerial_perspective_;
  Clouds clouds_;
  Precipitation precipitation_;
  SurfaceWeather surface_weather_;
  ParticleSystem particles_;
  GaussianSplat gaussians_;
  FurPass fur_;
  WboitPass wboit_;
  OverdrawPass overdraw_;
  GpuCull gpu_cull_;
  MeshletPass meshlet_;
  GpuImage ms_dummy_hiz_;  // 1x1 fallback bound to the mesh-shader cull when occlusion is off
  Mat4 pt_prev_view_proj_ = Mat4::Identity();
  f32 pt_prev_sig_ = 0;  // lighting signature; change resets accumulation
  bool pt_was_active_ = false;
  // Which path-trace mode ran last frame (0 reference, 1 nrd-denoised, 2 recon,
  // -1 none). Switching mode must reset accumulation: each mode reprojects its
  // own history buffers, which the other modes never wrote.
  int pt_prev_mode_ = -1;
  // A no_rt (foliage) mesh was uploaded while path tracing was off, so it has no
  // blas yet; EnsureRayTracingGeometry catches it up when path tracing turns on.
  bool rt_foliage_dirty_ = false;

  // Settings already in effect, diffed against settings_ each frame.
  UpscalerKind applied_upscaler_ = UpscalerKind::kNone;
  UpscalerQuality applied_quality_ = UpscalerQuality::kQuality;
  f32 applied_render_scale_ = 1.0f;
  AntiAliasingMode applied_aa_ = AntiAliasingMode::kTaa;
  bool applied_vsync_ = false;
  // Sun state baked into the environment maps; differing means regenerate.
  Vec3 applied_sun_direction_{};
  f32 applied_sun_intensity_ = -1;
  Vec3 applied_sun_color_{};
  bool environment_dirty_ = true;

  void WriteScreenshot(u32 image_index);
  void WriteHdr();  // reads back the captured linear hdr buffer to a .hdr file

  std::string screenshot_path_;
  f64 screenshot_at_ = -1;  // seconds; <0 means immediately when armed

  // Linear-hdr frame export (radiance .hdr). REC_HDR=<path>[:seconds].
  std::string hdr_path_;
  f64 hdr_at_ = -1;
  bool hdr_pending_ = false;  // the copy pass ran this frame; read it back after submit
  u32 hdr_width_ = 0, hdr_height_ = 0;
  GpuBuffer hdr_readback_;  // host-visible rgba32f, one float4 per pixel
  VkDescriptorSetLayout hdr_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout hdr_layout_ = VK_NULL_HANDLE;
  VkPipeline hdr_pipeline_ = VK_NULL_HANDLE;
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
  u32 meshlet_visible_ = 0;      // survivors of the last meshlet cluster cull (fence-safe)
  u32 render_width_ = 0;
  u32 render_height_ = 0;
  u32 output_width_ = 0;
  u32 output_height_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDERER_H_

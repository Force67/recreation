#include "render/core/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <base/option.h>

#include <stb_image_write.h>

#include "asset/primitives.h"
#include "core/log.h"
#include "render/util/exr_write.h"
#include "shaders/hdr_capture_cs_hlsl.h"
#include "shaders/cloud_shadow_cs_hlsl.h"
#include "shaders/contact_shadow_cs_hlsl.h"
#include "shaders/light_cluster_cs_hlsl.h"
#include "shaders/sss_blur_cs_hlsl.h"

namespace rec::render {
namespace {

// Renderer config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup (see Engine::Initialize). DebugView and
// ColorGrade options take an Opt suffix to avoid shadowing the enums of the same
// name used in the casts below.
base::Option<const char*> Screenshot{"screenshot", nullptr, "REC_SCREENSHOT"};
base::Option<const char*> Hdr{"hdr", nullptr, "REC_HDR"};
base::Option<bool> HdrOutput{"hdr.output", false, "REC_HDR_OUTPUT"};
base::Option<bool> MotionBlurOpt{"motion.blur", true, "REC_MOTION_BLUR"};
base::Option<bool> DofOpt{"dof", true, "REC_DOF"};
base::Option<double> LensFlareOpt{"lens.flare", 0.06, "REC_LENS_FLARE"};
base::Option<double> GrainOpt{"film.grain", 0.015, "REC_FILM_GRAIN"};
base::Option<double> DofFocus{"dof.focus", 0.0, "REC_DOF_FOCUS"};
base::Option<double> DofAperture{"dof.aperture", 2.8, "REC_DOF_APERTURE"};
base::Option<bool> SssOpt{"sss", true, "REC_SSS"};
base::Option<double> SssWidth{"sss.width", 0.012, "REC_SSS_WIDTH"};
base::Option<bool> AsyncComputeOpt{"async.compute", true, "REC_ASYNC_COMPUTE"};
base::Option<bool> FrameGenOpt{"framegen", false, "REC_FRAMEGEN"};
base::Option<bool> LocalShadowsOpt{"local.shadows", true, "REC_LOCAL_SHADOWS"};
base::Option<bool> FroxelOpt{"froxel.fog", true, "REC_FROXEL"};
base::Option<double> FroxelDensity{"froxel.density", 0.005, "REC_FROXEL_DENSITY"};
base::Option<bool> VrsOpt{"vrs", true, "REC_VRS"};
base::Option<double> VrsThreshold{"vrs.threshold", 0.06, "REC_VRS_THRESHOLD"};
base::Option<bool> RestirDiOpt{"restir.di", false, "REC_RESTIR_DI"};
base::Option<float> VgeoError{"vgeo.error", 1.0f, "REC_VGEO_ERROR"};
base::Option<bool> FftOceanOpt{"fft.ocean", true, "REC_FFT_OCEAN"};
// Debug: horizontal fake velocity in pixels, to exercise the blur from a
// static camera (screenshot testing).
base::Option<double> MotionBlurDebugVel{"motion.blur.debug.vel", 0.0, "REC_MOTION_BLUR_DEBUG_VEL"};
base::Option<double> HdrPaperWhite{"hdr.paper.white", 200.0, "REC_HDR_PAPER_WHITE"};
// Debug: force the tonemap's output transfer (1 pq, 2 scrgb) on an SDR
// swapchain, so the encode math is testable on displays with no HDR path.
base::Option<int> HdrForceTransfer{"hdr.force.transfer", 0, "REC_HDR_FORCE_TRANSFER"};
base::Option<bool> Wireframe{"wireframe", false, "REC_WIREFRAME"};
base::Option<bool> Ssr{"ssr", false, "REC_SSR"};
base::Option<bool> Ssgi{"ssgi", false, "REC_SSGI"};
base::Option<bool> DistanceLod{"distance.lod", false, "REC_DISTANCE_LOD"};
base::Option<bool> MeshShaderLod{"mesh.shader.lod", false, "REC_MESH_SHADER_LOD"};
base::Option<int> DebugViewOpt{"debug.view", 0, "REC_DEBUG_VIEW"};
base::Option<int> ColorGradeOpt{"color.grade", 0, "REC_COLOR_GRADE"};
base::Option<const char*> Lut{"lut", nullptr, "REC_LUT"};
base::Option<const char*> SunDir{"sun.dir", nullptr, "REC_SUN_DIR"};
base::Option<bool> Pathtrace{"pathtrace", false, "REC_PATHTRACE"};
base::Option<bool> PathtraceReference{"pathtrace.reference", false, "REC_PATHTRACE_REFERENCE"};
base::Option<int> PathtraceSpp{"pathtrace.spp", 2, "REC_PATHTRACE_SPP"};
base::Option<int> PathtraceAccum{"pathtrace.accum", 16, "REC_PATHTRACE_ACCUM"};
base::Option<bool> PathtraceRecon{"pathtrace.recon", false, "REC_PATHTRACE_RECON"};
base::Option<int> PathtraceReconDebug{"pathtrace.recon.debug", 0, "REC_PATHTRACE_RECON_DEBUG"};
base::Option<bool> PathtraceRestir{"pathtrace.restir", true, "REC_PATHTRACE_RESTIR"};
base::Option<bool> PathtraceRestirDi{"pathtrace.restir.di", true, "REC_PATHTRACE_RESTIR_DI"};
base::Option<bool> PathtraceRr{"pathtrace.rr", true, "REC_PATHTRACE_RR"};
base::Option<bool> Fog{"fog", false, "REC_FOG"};
base::Option<float> Aerial{"aerial", 1.0f, "REC_AERIAL"};
base::Option<bool> CloudsOpt{"clouds", false, "REC_CLOUDS"};
base::Option<float> CloudCoverage{"cloud.coverage", 0.46f, "REC_CLOUD_COVERAGE"};
base::Option<float> Precip{"precip", 0.0f, "REC_PRECIP"};
base::Option<bool> Snow{"snow", false, "REC_SNOW"};
base::Option<bool> Aurora{"aurora", false, "REC_AURORA"};
base::Option<const char*> RhiBackend{"rhi.backend", nullptr, "REC_RHI"};

// Distance-based hierarchical lod: coarser geometry the further a mesh is from
// the camera. Switches roughly every few bounding radii; clamps to the coarsest.
u32 SelectLod(const GpuMesh& mesh, f32 distance) {
  u32 lod_count = 1u + static_cast<u32>(mesh.lods.size());
  if (lod_count <= 1) return 0;
  f32 unit = std::max(mesh.bounds_radius, 0.25f) * 2.5f;
  u32 lod = static_cast<u32>(distance / std::max(unit, 0.5f));
  return lod < lod_count ? lod : lod_count - 1;
}

// Gribb-Hartmann frustum planes (left,right,bottom,top,near) from a column-major
// view_proj, normalized so a point is inside when dot(n,p)+d >= 0. Far is skipped.
void ExtractFrustumPlanes(const Mat4& vp, f32 out[5][4]) {
  const f32* m = vp.m;
  auto row = [&](int r, int c) { return m[c * 4 + r]; };
  f32 p[5][4] = {
      {row(3, 0) + row(0, 0), row(3, 1) + row(0, 1), row(3, 2) + row(0, 2), row(3, 3) + row(0, 3)},
      {row(3, 0) - row(0, 0), row(3, 1) - row(0, 1), row(3, 2) - row(0, 2), row(3, 3) - row(0, 3)},
      {row(3, 0) + row(1, 0), row(3, 1) + row(1, 1), row(3, 2) + row(1, 2), row(3, 3) + row(1, 3)},
      {row(3, 0) - row(1, 0), row(3, 1) - row(1, 1), row(3, 2) - row(1, 2), row(3, 3) - row(1, 3)},
      {row(2, 0), row(2, 1), row(2, 2), row(2, 3)},
  };
  for (int i = 0; i < 5; ++i) {
    f32 len = std::sqrt(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
    if (len < 1e-8f) len = 1.0f;
    for (int c = 0; c < 4; ++c) out[i][c] = p[i][c] / len;
  }
}

// World-space sphere vs the normalized frustum planes; outside if it falls beyond
// any plane. Lets the cpu skip a draw entirely (no dispatch) for off-screen
// instances, so cpu cost tracks visible instances rather than streamed ones.
bool SphereOutsideFrustum(const f32 planes[5][4], const Vec3& c, f32 r) {
  for (int i = 0; i < 5; ++i) {
    if (planes[i][0] * c.x + planes[i][1] * c.y + planes[i][2] * c.z + planes[i][3] < -r) {
      return true;
    }
  }
  return false;
}

}  // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Initialize(const RendererDesc& desc, Window& window) {
  desc_ = desc;
  settings_.aa_mode = desc.aa_mode;
  settings_.upscaler = desc.upscaler;
  settings_.rt_shadows = desc.raytracing.shadows;
  output_width_ = window.width();
  output_height_ = window.height();
  // Applied before the swapchain exists (the rest of the option overrides run
  // later in Initialize; these two decide the surface format).
  if (HdrOutput.overridden()) settings_.hdr_output = HdrOutput;
  if (HdrPaperWhite.overridden()) settings_.hdr_paper_white = static_cast<f32>(double(HdrPaperWhite));

  // REC_RHI=vulkan|d3d12|null|auto overrides the graphics backend.
  Backend backend = desc.backend;
  if (const char* name = RhiBackend.get()) {
    std::string value = name;
    if (value == "vulkan") backend = Backend::kVulkan;
    else if (value == "d3d12") backend = Backend::kD3D12;
    else if (value == "null") backend = Backend::kNull;
    else if (value == "auto") backend = Backend::kAuto;
    else REC_WARN("REC_RHI: unknown backend '{}', using {}", value, BackendName(backend));
  }

  window_ = &window;
  device_ = Device::Create({.backend = backend,
                            .enable_validation = desc.enable_validation,
                            .request_raytracing = desc.enable_raytracing},
                           window);
  if (device_->is_stub()) {
    REC_WARN("renderer running in stub mode");
    return true;
  }

  // Everything below creates pipelines; batch them so the driver compiles
  // across cores on a cold cache (the guard joins the workers on every exit
  // path, and a failed compile fails Initialize at the End check).
  struct PipelineBatchGuard {
    Device& device;
    bool ended = false;
    bool End() {
      ended = true;
      return device.EndPipelineBatch();
    }
    ~PipelineBatchGuard() {
      if (!ended) device.EndPipelineBatch();
    }
  } pipeline_batch{*device_};
  auto t_batch0 = std::chrono::steady_clock::now();
  const char* pso_batch_env = std::getenv("REC_PSO_BATCH");
  if (!pso_batch_env || pso_batch_env[0] != '0') device_->BeginPipelineBatch();

  swapchain_ = device_->CreateSwapchain(output_width_, output_height_, settings_.vsync,
                                        settings_.hdr_output);
  if (!swapchain_ || !CreateFrameResources()) return false;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  if (desc.enable_raytracing && device_->caps().raytracing) {
    raytracing_ = RayTracingContext::Create(*device_);
    raytracing_->Configure(desc.raytracing);
  }
  rt_available_ = raytracing_ && device_->caps().ray_query;

  transient_pool_ = std::make_unique<TransientPool>(*device_);
  if (rt_available_) {
    bindless_ = BindlessRegistry::Create(*device_);
    if (!bindless_) return false;
  }
  material_system_ = MaterialSystem::Create(*device_, bindless_.get());
  if (!material_system_) return false;
  environment_ = EnvironmentSystem::Create(*device_);
  if (!environment_) return false;
  mesh_pipeline_ = MeshPipeline::Create(*device_, kSceneColorFormat, kMotionFormat,
                                        kNormalFormat, kDepthFormat,
                                        material_system_->set_layout(),
                                        environment_->env_set_layout(),
                                        bindless_ ? bindless_->set_layout() : BindingLayoutHandle{});
  post_ = PostPass::Create(*device_, swapchain_->format());
  if (!mesh_pipeline_ || !post_ || !taa_.Initialize(*device_)) return false;
  ui_blur_ = UiBlurPass::Create(*device_);  // optional: frosted-glass UI blur
  if (rt_available_ && !rtao_.Initialize(*device_)) return false;
  if (rt_available_ && bindless_ &&
      !reflection_trace_.Initialize(*device_, bindless_->set_layout())) {
    return false;
  }
  if (!motion_blur_.Initialize(*device_)) return false;
  if (!dof_.Initialize(*device_)) return false;
  {
    struct ClusterPush {
      Mat4 view;
      f32 screen[2];
      f32 near_plane;
      f32 slice_scale;
      f32 slice_bias;
      u32 light_count;
      f32 tan_half_fov_y;
      f32 aspect;
      u32 decal_count;
      f32 pad[3];
    };
    light_cluster_pipeline_ = device_->CreateComputePipeline({
        .shader = REC_SHADER(k_light_cluster_cs_hlsl),
        .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                            {1, BindingType::kStorageBuffer},
                            {2, BindingType::kStorageBuffer},
                            {3, BindingType::kStorageBuffer},
                            {4, BindingType::kStorageBuffer}}}},
        .push_constant_size = sizeof(ClusterPush),
        .debug_name = "light_cluster",
    });
    if (!light_cluster_pipeline_) return false;
    struct ContactPush {
      Mat4 view_proj;
      Mat4 inv_view_proj;
      f32 sun_dir[3];
      f32 near_plane;
      u32 size[2];
      f32 range;
      f32 thickness;
      u32 steps;
      u32 frame_index;
      f32 pad[2];
    };
    contact_shadow_pipeline_ = device_->CreateComputePipeline({
        .shader = REC_SHADER(k_contact_shadow_cs_hlsl),
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kSampledImage}}}},
        .push_constant_size = sizeof(ContactPush),
        .debug_name = "contact_shadow",
    });
    if (!contact_shadow_pipeline_) return false;
    struct CloudShadowPush {
      Mat4 inv_view_proj;
      f32 sun_dir[3];
      f32 near_plane;
      u32 size[2];
      f32 time;
      f32 coverage;
      f32 bottom;
      f32 top;
      f32 wind;
      f32 strength;
    };
    cloud_shadow_pipeline_ = device_->CreateComputePipeline({
        .shader = REC_SHADER(k_cloud_shadow_cs_hlsl),
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kSampledImage}}}},
        .push_constant_size = sizeof(CloudShadowPush),
        .debug_name = "cloud_shadow",
    });
    if (!cloud_shadow_pipeline_) return false;
    struct SssPush {
      u32 size[2];
      f32 inv_size[2];
      f32 dir[2];
      f32 near_plane;
      f32 width;
      f32 proj_scale;
      f32 max_radius;
      u32 composite;
      f32 strength;
    };
    sss_pipeline_ = device_->CreateComputePipeline({
        .shader = REC_SHADER(k_sss_blur_cs_hlsl),
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kCombinedTextureSampler},
                            {2, BindingType::kCombinedTextureSampler},
                            {3, BindingType::kCombinedTextureSampler}}}},
        .push_constant_size = sizeof(SssPush),
        .debug_name = "sss_blur",
    });
    if (!sss_pipeline_) return false;
    sss_sampler_ = device_->GetSampler({.address_u = AddressMode::kClampToEdge,
                                        .address_v = AddressMode::kClampToEdge});
    cluster_counts_ = device_->CreateBuffer(kClusterCount * sizeof(u32), kBufferUsageStorage);
    cluster_indices_ = device_->CreateBuffer(
        static_cast<u64>(kClusterCount) * kMaxLightsPerCluster * sizeof(u32),
        kBufferUsageStorage);
    decal_cluster_indices_ = device_->CreateBuffer(
        static_cast<u64>(kClusterCount) * kMaxDecalsPerCluster * sizeof(u32),
        kBufferUsageStorage);
    if (!cluster_counts_ || !cluster_indices_ || !decal_cluster_indices_) return false;
  }
  if (!ssao_.Initialize(*device_)) return false;  // raster ao fallback, no rt needed
  if (!ssr_.Initialize(*device_)) return false;   // raster reflection fallback
  if (!ssgi_.Initialize(*device_)) return false;  // raster diffuse-gi fallback

  // Persistent per-slot sets for the frame globals and environment bindings.
  // Contents are rewritten each frame after the slot's fence has fired, before
  // any pass of the new frame binds them.
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    globals_sets_[i] = device_->CreateBindingSet(mesh_pipeline_->set_layout());
    env_scene_sets_[i] = device_->CreateBindingSet(environment_->env_set_layout());
    env_transparent_sets_[i] = device_->CreateBindingSet(environment_->env_set_layout());
    env_prepass_sets_[i] = device_->CreateBindingSet(environment_->env_set_layout());
    if (!globals_sets_[i] || !env_scene_sets_[i] || !env_transparent_sets_[i] ||
        !env_prepass_sets_[i]) {
      return false;
    }
  }

  // Linear-hdr export: a compute copy from the resolved scene into a host buffer.
  hdr_pipeline_ = device_->CreateComputePipeline({
      .shader = REC_SHADER(k_hdr_capture_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}, {1, BindingType::kSampledImage}}}},
      .push_constant_size = 2 * sizeof(u32),
      .debug_name = "hdr_capture",
  });
  if (!hdr_pipeline_) return false;

  if (!shadow_.Initialize(*device_, material_system_->set_layout())) return false;  // raster sun shadows
  if (!local_shadows_.Initialize(*device_)) return false;  // clustered light shadows
  if (!froxel_fog_.Initialize(*device_)) {
    REC_WARN("froxel volumetrics unavailable");  // non-fatal: feature gates on available()
  }
  vrs_.Initialize(*device_);  // non-fatal: needs attachment VRS hardware
  if (rt_available_) restir_di_.Initialize(*device_);  // non-fatal: gates on available()
  virtual_texture_.Initialize(*device_);  // non-fatal: gates on available()
  if (!particles_.Initialize(*device_, kSceneColorFormat,
                             bindless_ ? bindless_->set_layout() : BindingLayoutHandle{}))
    return false;
  if (!gaussians_.Initialize(*device_, kSceneColorFormat)) return false;
  if (!fur_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!wboit_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!overdraw_.Initialize(*device_, kSceneColorFormat)) return false;
  if (!gpu_cull_.Initialize(*device_, kSceneColorFormat)) return false;
  if (!meshlet_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!vgeo_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!hair_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!imposters_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!ocean_.Initialize(*device_)) {
    REC_WARN("fft ocean unavailable");  // non-fatal: gerstner fallback
  }
  if (device_->caps().mesh_shaders) {
    // 1x1 fallback hi-z so the mesh-shader cull descriptor is always valid; bound
    // (with occlusion disabled) on frames where no real hi-z was built.
    ms_dummy_hiz_ = device_->CreateImage2D(Format::kR32Float, {1, 1}, kTextureUsageSampled);
    device_->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(ms_dummy_hiz_, ResourceState::kUndefined,
                             ResourceState::kShaderReadAll));
    });
  }
  if (!bloom_.Initialize(*device_) || !exposure_.Initialize(*device_)) return false;
  if (rt_available_) {
    ddgi_ = DdgiSystem::Create(*device_, environment_->sky_view(), environment_->sampler(),
                               *bindless_);
    if (!ddgi_) return false;
    water_ = WaterPass::Create(*device_, kSceneColorFormat, kMotionFormat, kDepthFormat,
                               mesh_pipeline_->set_layout(), material_system_->set_layout(),
                               environment_->env_set_layout(), bindless_->set_layout());
    if (!water_) return false;
  }
  if (!environment_->CreateSkyPipeline(mesh_pipeline_->set_layout(), kSceneColorFormat,
                                       kMotionFormat, kDepthFormat)) {
    return false;
  }

  if (settings_.upscaler != UpscalerKind::kNone && !CreateUpscalerForSettings()) {
    REC_WARN("upscaler unavailable, falling back to taa");
    settings_.upscaler = UpscalerKind::kNone;
    settings_.aa_mode = AntiAliasingMode::kTaa;
  }
  applied_upscaler_ = settings_.upscaler;
  applied_quality_ = settings_.upscaler_quality;
  applied_aa_ = settings_.aa_mode;
  applied_vsync_ = settings_.vsync;

  profiler_.Initialize(*device_, kFramesInFlight);
  if (rt_available_ && bindless_) {
    path_tracer_.Initialize(*device_, bindless_->set_layout());
    recon_path_tracer_.Initialize(*device_, bindless_->set_layout());
  }
  if (rt_available_) volumetric_fog_.Initialize(*device_);
  aerial_perspective_.Initialize(*device_);  // atmospheric distance haze (no ray tracing)
  clouds_.Initialize(*device_);              // volumetric clouds (no ray tracing)
  precipitation_.Initialize(*device_);       // screen-space rain/snow
  surface_weather_.Initialize(*device_);     // rain wetness / snow accumulation

  UpdateRenderResolution();
  vrs_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_) restir_di_.Resize(*device_, {render_width_, render_height_});
  taa_.Resize(*device_, {render_width_, render_height_});
  ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
  path_tracer_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_ && settings_.path_trace_recon) {
    recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
  }
  if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
#if defined(RECREATION_HAS_NRD)
  if (rt_available_ && !nrd_.Initialize(*device_, {render_width_, render_height_})) {
    REC_WARN("nrd denoiser unavailable, rtao/shadow denoising disabled");
  }
  if (rt_available_ && !shadow_trace_.Initialize(*device_)) {
    REC_WARN("shadow trace unavailable, sigma sun-shadow denoising disabled");
  }
  if (rt_available_) shadow_trace_.Resize(*device_, {render_width_, render_height_});
#endif

  // Debug captures without window manager screenshots:
  // REC_SCREENSHOT=/tmp/frame.png:12 saves the frame at t=12s.
  if (const char* spec = Screenshot.get()) {
    std::string value = spec;
    size_t colon = value.find_last_of(':');
    if (colon != std::string::npos) {
      screenshot_at_ = std::atof(value.c_str() + colon + 1);
      value.resize(colon);
    }
    screenshot_path_ = value;
  }

  // REC_HDR=/tmp/frame.hdr:12 exports the linear-hdr frame (radiance rgbe) at t=12s.
  if (const char* spec = Hdr.get()) {
    std::string value = spec;
    size_t colon = value.find_last_of(':');
    if (colon != std::string::npos) {
      hdr_at_ = std::atof(value.c_str() + colon + 1);
      value.resize(colon);
    }
    hdr_path_ = value;
  }

  if (Wireframe.overridden()) settings_.wireframe = Wireframe;
  if (Ssr.overridden()) settings_.ssr = Ssr;
  if (Ssgi.overridden()) settings_.ssgi = Ssgi;
  // REC_DISTANCE_LOD=1 re-enables distance-based lod downgrade (off by default;
  // the engine otherwise always renders the finest authored detail).
  if (DistanceLod.overridden()) settings_.distance_lod = DistanceLod;
  // REC_MESH_SHADER_LOD=1 opts into the optional mesh-shader opaque path.
  if (MeshShaderLod.overridden()) settings_.mesh_shader_lod = MeshShaderLod;
  // Hardware gate: the path needs mesh shaders and its pipelines to have
  // built. Disable + warn rather than silently doing nothing if it was requested.
  bool mesh_shader_ok = device_->caps().mesh_shaders && mesh_pipeline_ &&
                        mesh_pipeline_->has_mesh_shader();
  if (mesh_shader_ok) {
    REC_INFO("mesh-shader lod path available (default {})", settings_.mesh_shader_lod ? "on" : "off");
  } else {
    if (settings_.mesh_shader_lod) {
      REC_WARN("mesh-shader lod requested but unavailable on this gpu, disabling");
    }
    settings_.mesh_shader_lod = false;
  }

  // REC_DEBUG_VIEW=<n> pins a debug channel at startup for headless capture;
  // exposure is fixed so the channel reads at its true magnitude.
  if (DebugViewOpt.overridden()) {
    settings_.debug_view = static_cast<DebugView>(DebugViewOpt.get());
    if (settings_.debug_view != DebugView::kOff) {
      settings_.auto_exposure = false;
      settings_.exposure = 1.0f;
    }
  }
  if (ColorGradeOpt.overridden()) {
    settings_.color_grade = static_cast<ColorGrade>(ColorGradeOpt.get());
  }
  // REC_LUT=<path> loads an external .cube 3D lut as the active color grade.
  if (const char* lut = Lut.get()) {
    if (post_ && post_->LoadCubeLut(lut)) settings_.color_grade = ColorGrade::kCustom;
  }
  // REC_SUN_DIR="x,y,z" overrides the sun travel direction, for headless
  // lighting/shadow tests (normalized; y clamped below the horizon).
  if (const char* sd = SunDir.get()) {
    Vec3 d{};
    if (std::sscanf(sd, "%f,%f,%f", &d.x, &d.y, &d.z) == 3) {
      f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      if (len > 1e-4f) settings_.sun_direction = {d.x / len, d.y / len, d.z / len};
    }
  }
  if (Pathtrace.overridden()) settings_.path_trace = Pathtrace;
  if (PathtraceReference.overridden()) settings_.path_trace_reference = PathtraceReference;
  if (MotionBlurOpt.overridden()) settings_.motion_blur = MotionBlurOpt;
  if (DofOpt.overridden()) settings_.dof = DofOpt;
  if (LensFlareOpt.overridden()) settings_.lens_flare = static_cast<f32>(double(LensFlareOpt));
  if (GrainOpt.overridden()) settings_.film_grain = static_cast<f32>(double(GrainOpt));
  if (DofFocus.overridden()) settings_.dof_focus = static_cast<f32>(double(DofFocus));
  if (DofAperture.overridden()) settings_.dof_aperture = static_cast<f32>(double(DofAperture));
  if (SssOpt.overridden()) settings_.sss = SssOpt;
  if (SssWidth.overridden()) settings_.sss_width = static_cast<f32>(double(SssWidth));
  if (AsyncComputeOpt.overridden()) settings_.async_compute = AsyncComputeOpt;
  if (FrameGenOpt.overridden()) settings_.frame_generation = FrameGenOpt;
  if (LocalShadowsOpt.overridden()) settings_.local_shadows = LocalShadowsOpt;
  if (FroxelOpt.overridden()) settings_.froxel_fog = FroxelOpt;
  if (FroxelDensity.overridden()) settings_.froxel_density = static_cast<f32>(double(FroxelDensity));
  if (VrsOpt.overridden()) settings_.vrs = VrsOpt;
  if (VrsThreshold.overridden()) settings_.vrs_threshold = static_cast<f32>(double(VrsThreshold));
  if (RestirDiOpt.overridden()) settings_.restir_di = RestirDiOpt;
  if (FftOceanOpt.overridden()) settings_.fft_ocean = FftOceanOpt;
  if (PathtraceSpp.overridden()) settings_.path_trace_spp = static_cast<u32>(std::max(1, int(PathtraceSpp)));
  if (PathtraceAccum.overridden()) settings_.path_trace_accum = static_cast<u32>(std::max(1, int(PathtraceAccum)));
  if (PathtraceRecon.overridden()) settings_.path_trace_recon = PathtraceRecon;
  if (PathtraceReconDebug.overridden()) settings_.path_trace_recon_debug = static_cast<u32>(std::max(0, int(PathtraceReconDebug)));
  if (PathtraceRestir.overridden()) settings_.path_trace_restir = PathtraceRestir;
  if (PathtraceRestirDi.overridden()) settings_.path_trace_restir_di = PathtraceRestirDi;
  if (PathtraceRr.overridden()) settings_.path_trace_rr = PathtraceRr;
  if (Fog.overridden()) settings_.fog = Fog;
  // REC_AERIAL overrides aerial-perspective strength (0 off, 1 physical, >1 exaggerated).
  if (Aerial.overridden()) settings_.aerial_perspective = Aerial.get();
  if (CloudsOpt.overridden()) settings_.clouds = CloudsOpt;
  if (CloudCoverage.overridden()) settings_.cloud_coverage = CloudCoverage.get();
  // REC_PRECIP forces precipitation (0..1) and REC_SNOW=1 makes it snow, so the
  // effect is testable without a loaded game's weather.
  if (Precip.overridden()) settings_.precipitation = Precip.get();
  if (Snow.overridden()) settings_.precip_snow = Snow;
  if (Aurora.overridden()) settings_.aurora = Aurora;

  auto t_batch1 = std::chrono::steady_clock::now();
  if (!pipeline_batch.End()) {
    REC_ERROR("pipeline batch reported failed compilations");
    return false;
  }
  auto t_batch2 = std::chrono::steady_clock::now();
  REC_INFO("renderer init: {} ms (pipeline batch joined in {} ms)",
           std::chrono::duration_cast<std::chrono::milliseconds>(t_batch1 - t_batch0).count(),
           std::chrono::duration_cast<std::chrono::milliseconds>(t_batch2 - t_batch1).count());
  return true;
}

void Renderer::CaptureScreenshot(const std::string& path) {
  screenshot_path_ = path;
  screenshot_at_ = -1;
}

void Renderer::WriteHdr() {
  device_->WaitIdle();  // the capture compute wrote hdr_readback_; drain before reading
  const f32* src = static_cast<const f32*>(hdr_readback_.mapped);
  if (!src) {
    hdr_path_.clear();
    return;
  }
  base::Vector<f32> rgb(static_cast<size_t>(hdr_width_) * hdr_height_ * 3);
  for (size_t i = 0; i < static_cast<size_t>(hdr_width_) * hdr_height_; ++i) {
    rgb[i * 3 + 0] = src[i * 4 + 0];
    rgb[i * 3 + 1] = src[i * 4 + 1];
    rgb[i * 3 + 2] = src[i * 4 + 2];
  }
  // .exr (OpenEXR float) is the production container; .hdr (radiance rgbe) is the
  // default. Both store the same linear pre-tonemap scene.
  bool is_exr = hdr_path_.size() >= 4 &&
                hdr_path_.compare(hdr_path_.size() - 4, 4, ".exr") == 0;
  bool ok = is_exr ? WriteExrRgbF32(hdr_path_, hdr_width_, hdr_height_, rgb.data())
                   : stbi_write_hdr(hdr_path_.c_str(), static_cast<int>(hdr_width_),
                                    static_cast<int>(hdr_height_), 3, rgb.data()) != 0;
  if (ok) {
    REC_INFO("{} frame written: {} ({}x{})", is_exr ? "exr" : "hdr", hdr_path_, hdr_width_,
             hdr_height_);
  } else {
    REC_WARN("hdr write failed: {}", hdr_path_);
  }
  hdr_path_.clear();
}

// Debug readback for frame generation verification (REC_FRAMEGEN_DUMP): the
// interpolated image should land between the two real frames around it.
void Renderer::DumpFgImage(const GpuImage& image, ResourceState state, bool bgra,
                           const char* path) {
  device_->WaitIdle();
  u64 size = static_cast<u64>(image.extent.width) * image.extent.height * 4;
  GpuBuffer staging = device_->CreateBuffer(size, kBufferUsageTransferDst, true);
  if (!staging.mapped) return;
  device_->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(image, state, ResourceState::kCopySrc));
    cmd.CopyTextureToBuffer(image, staging, {});
    cmd.Barrier(Transition(image, ResourceState::kCopySrc, state));
  });
  base::Vector<u8> pixels(static_cast<size_t>(image.extent.width) * image.extent.height * 3);
  const u8* src = static_cast<const u8*>(staging.mapped);
  for (size_t i = 0; i < static_cast<size_t>(image.extent.width) * image.extent.height; ++i) {
    pixels[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
    pixels[i * 3 + 1] = src[i * 4 + 1];
    pixels[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
  }
  device_->DestroyBuffer(staging);
  if (stbi_write_png(path, static_cast<int>(image.extent.width),
                     static_cast<int>(image.extent.height), 3, pixels.data(),
                     static_cast<int>(image.extent.width) * 3)) {
    REC_INFO("framegen dump written: {}", path);
  }
}

void Renderer::WriteScreenshot(u32 image_index) {
  device_->WaitIdle();
  Extent2D extent = swapchain_->extent();
  u64 size = static_cast<u64>(extent.width) * extent.height * 4;
  GpuBuffer staging = device_->CreateBuffer(size, kBufferUsageTransferDst, true);
  if (!staging.mapped) return;

  const GpuImage& backbuffer = swapchain_->image(image_index);
  device_->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(backbuffer, ResourceState::kPresent, ResourceState::kCopySrc));
    cmd.CopyTextureToBuffer(backbuffer, staging, {});
    cmd.Barrier(Transition(backbuffer, ResourceState::kCopySrc, ResourceState::kPresent));
  });

  // Swapchain is bgra; png wants rgb.
  base::Vector<u8> pixels(static_cast<size_t>(extent.width) * extent.height * 3);
  const u8* src = static_cast<const u8*>(staging.mapped);
  for (size_t i = 0; i < static_cast<size_t>(extent.width) * extent.height; ++i) {
    pixels[i * 3 + 0] = src[i * 4 + 2];
    pixels[i * 3 + 1] = src[i * 4 + 1];
    pixels[i * 3 + 2] = src[i * 4 + 0];
  }
  device_->DestroyBuffer(staging);
  if (stbi_write_png(screenshot_path_.c_str(), static_cast<int>(extent.width),
                     static_cast<int>(extent.height), 3, pixels.data(),
                     static_cast<int>(extent.width) * 3)) {
    REC_INFO("screenshot written: {}", screenshot_path_);
  } else {
    REC_WARN("screenshot write failed: {}", screenshot_path_);
  }
  screenshot_path_.clear();
}

bool Renderer::CreateUpscalerForSettings() {
  f32 scale = UpscalerScale(settings_.upscaler_quality);
  u32 render_width = static_cast<u32>(static_cast<f32>(output_width_) / scale);
  u32 render_height = static_cast<u32>(static_cast<f32>(output_height_) / scale);
  upscaler_ = CreateUpscaler({.kind = settings_.upscaler,
                              .render_width = render_width,
                              .render_height = render_height,
                              .output_width = output_width_,
                              .output_height = output_height_,
                              .sharpness = settings_.sharpness},
                             *device_);
  if (upscaler_) {
    settings_.aa_mode = AntiAliasingMode::kUpscaler;
    return true;
  }
  return false;
}

void Renderer::UpdateRenderResolution() {
  if (upscaler_ && settings_.aa_mode == AntiAliasingMode::kUpscaler) {
    f32 scale = UpscalerScale(settings_.upscaler_quality);
    render_width_ = static_cast<u32>(static_cast<f32>(output_width_) / scale);
    render_height_ = static_cast<u32>(static_cast<f32>(output_height_) / scale);
  } else {
    // No upscaler: render at output * render_scale. >1 supersamples (the post
    // pass samples this image into the swapchain, so it downscales for free).
    f32 rs = std::clamp(settings_.render_scale, 0.25f, 2.0f);
    render_width_ = std::max(1u, static_cast<u32>(static_cast<f32>(output_width_) * rs));
    render_height_ = std::max(1u, static_cast<u32>(static_cast<f32>(output_height_) * rs));
  }
}

void Renderer::ResizeSizedPasses() {
  vrs_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_) restir_di_.Resize(*device_, {render_width_, render_height_});
  taa_.Resize(*device_, {render_width_, render_height_});
  ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
  path_tracer_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_ && settings_.path_trace_recon) {
    recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
  }
  if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
#if defined(RECREATION_HAS_NRD)
  if (rt_available_ && nrd_.available()) nrd_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_) shadow_trace_.Resize(*device_, {render_width_, render_height_});
#endif
#if defined(RECREATION_HAS_DLSS)
  rr_.Resize(*device_, {render_width_, render_height_});
#endif
}

void Renderer::ApplySettings() {
  if (settings_.vsync != applied_vsync_) {
    applied_vsync_ = settings_.vsync;
    RecreateSwapchain();
  }

  // kUpscaler is only valid with a live upscaler.
  if (settings_.aa_mode == AntiAliasingMode::kUpscaler && settings_.upscaler == UpscalerKind::kNone) {
    settings_.aa_mode = AntiAliasingMode::kTaa;
  }

  bool upscaler_changed = settings_.upscaler != applied_upscaler_ ||
                          settings_.upscaler_quality != applied_quality_ ||
                          settings_.render_scale != applied_render_scale_;
  if (upscaler_changed) {
    device_->WaitIdle();
    upscaler_.reset();
    if (settings_.upscaler != UpscalerKind::kNone) {
      if (!CreateUpscalerForSettings()) {
        REC_WARN("upscaler unavailable, falling back to taa");
        settings_.upscaler = UpscalerKind::kNone;
        settings_.aa_mode = AntiAliasingMode::kTaa;
      }
    } else if (settings_.aa_mode == AntiAliasingMode::kUpscaler) {
      settings_.aa_mode = AntiAliasingMode::kTaa;
    }
    applied_upscaler_ = settings_.upscaler;
    applied_quality_ = settings_.upscaler_quality;
    applied_render_scale_ = settings_.render_scale;
    UpdateRenderResolution();
    transient_pool_->Clear();
    ResizeSizedPasses();
    taa_.Reset();
    has_prev_frame_ = false;
  }

  if (settings_.aa_mode != applied_aa_) {
    bool resolution_changes = settings_.aa_mode == AntiAliasingMode::kUpscaler ||
                              applied_aa_ == AntiAliasingMode::kUpscaler;
    applied_aa_ = settings_.aa_mode;
    if (resolution_changes) {
      device_->WaitIdle();
      UpdateRenderResolution();
      transient_pool_->Clear();
      ResizeSizedPasses();
    }
    taa_.Reset();
    has_prev_frame_ = false;
  }

  taa_.Configure({.history_blend = settings_.taa_history_blend,
                  .jitter_sample_count = taa_.settings().jitter_sample_count});
  rtao_.Configure({.radius = settings_.ao_radius,
                   .ray_count = settings_.ao_rays == 0 ? 1 : settings_.ao_rays});
  ssao_.Configure({.radius = settings_.ao_radius,
                   .intensity = settings_.ao_intensity * 1.8f,
                   .power = 1.5f,
                   .sample_count = std::clamp(settings_.ao_rays * 8u, 4u, 32u)});
  shadow_.Configure({.cascade_count = ShadowPass::kMaxCascades,
                     .resolution = settings_.shadow_resolution,
                     .distance = settings_.shadow_distance});
  exposure_.Configure({.automatic = settings_.auto_exposure,
                       .compensation = settings_.exposure,
                       .adaptation_speed = settings_.adaptation_speed,
                       .manual_exposure = settings_.exposure});
  if (ddgi_) {
    ddgi_->Configure({.probe_spacing = settings_.ddgi_spacing,
                      .hysteresis = 0.97f,
                      .energy_scale = settings_.ddgi_intensity});
  }

  Vec3 sun = Normalize(settings_.sun_direction);
  bool sun_changed = sun.x != applied_sun_direction_.x || sun.y != applied_sun_direction_.y ||
                     sun.z != applied_sun_direction_.z ||
                     settings_.sun_intensity != applied_sun_intensity_ ||
                     settings_.sun_color.x != applied_sun_color_.x ||
                     settings_.sun_color.y != applied_sun_color_.y ||
                     settings_.sun_color.z != applied_sun_color_.z;
  if (sun_changed) {
    applied_sun_direction_ = sun;
    applied_sun_intensity_ = settings_.sun_intensity;
    applied_sun_color_ = settings_.sun_color;
    environment_dirty_ = true;
  }
}

void Renderer::UploadMeshletMesh(const asset::Mesh& mesh) {
  if (!device_ || device_->is_stub()) return;
  meshlet_.Upload(*device_, mesh);
}

void Renderer::UploadVirtualGeometryMesh(const asset::Mesh& mesh) {
  if (!device_ || device_->is_stub()) return;
  vgeo_.Upload(*device_, mesh);
}

void Renderer::SeedHairStrands(const Vec3& head_center, f32 head_radius, u32 strands,
                               f32 length) {
  if (!device_ || device_->is_stub()) return;
  hair_.SeedCap(*device_, head_center, head_radius, strands, length);
}

void Renderer::BakeImposter(const asset::Mesh& mesh,
                            std::span<const ImposterPass::Instance> instances) {
  if (!device_ || device_->is_stub()) return;
  imposters_.Bake(*device_, mesh);
  imposters_.SetInstances(*device_, instances);
}

bool Renderer::UploadMesh(const asset::Mesh& mesh, u64 id_salt) {
  if (!device_ || device_->is_stub()) return false;
  const u64 mesh_key = mesh.id.hash ^ id_salt;
  // Resolve each emitter's texture asset hash to a bindless index (or invalid)
  // so the particle billboards can sample the authored effect texture.
  auto register_emitters = [&](const base::Vector<asset::ParticleEmitter>& src) {
    base::Vector<asset::ParticleEmitter> emitters = src;
    for (asset::ParticleEmitter& e : emitters) {
      u32 index = BindlessRegistry::kInvalidIndex;
      if (e.texture != 0 && material_system_) {
        index = material_system_->bindless_texture(e.texture ^ id_salt);
      }
      e.texture = index;  // now a bindless index, 0xffffffff = untextured
    }
    mesh_emitters_[mesh_key] = std::move(emitters);
  };
  // Emitter-only NIFs (smoke columns, dust wisps) carry no geometry but still
  // need their particle pools: register the emitters and accept the upload so
  // the placed reference spawns and the sim runs, without a GPU mesh.
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) {
    if (mesh.emitters.empty()) return false;
    register_emitters(mesh.emitters);
    return true;
  }

  BufferUsageFlags rt_usage =
      raytracing_ ? (kBufferUsageAccelBuildInput | kBufferUsageDeviceAddress) : 0;
  // The mesh-shader path reads vertices/meshlets by device address.
  const bool build_meshlets = device_->caps().mesh_shaders && !mesh.skinned;
  BufferUsageFlags ms_usage = build_meshlets ? kBufferUsageDeviceAddress : 0;

  // On the mesh-shader path, synthesize coarse lods for eligible single-material
  // statics so the task stage can drop detail with distance (GenerateLods is a
  // no-op for skinned / multi-submesh / already-multi-lod / tiny meshes). The
  // copy only happens for meshes that will actually get lods.
  asset::Mesh lodded;
  const asset::Mesh* src = &mesh;
  if (build_meshlets && mesh.lods.size() == 1 && mesh.lods[0].submeshes.size() <= 1 &&
      mesh.lods[0].indices.size() >= 3000) {
    lodded = mesh;
    asset::GenerateLods(&lodded);
    if (lodded.lods.size() > 1) src = &lodded;
  }

  const asset::MeshLod& lod = src->lods[0];
  GpuMesh gpu;

  // Concatenate every lod into shared vertex/index buffers; each lod keeps its
  // local indices, rebased onto its vertices through the draw's vertexOffset.
  base::Vector<asset::Vertex> all_verts;
  base::Vector<u32> all_indices;
  base::Vector<u32> vertex_bases, index_bases;
  for (const asset::MeshLod& l : src->lods) {
    vertex_bases.push_back(static_cast<u32>(all_verts.size()));
    index_bases.push_back(static_cast<u32>(all_indices.size()));
    for (const asset::Vertex& v : l.vertices) all_verts.push_back(v);
    for (u32 idx : l.indices) all_indices.push_back(idx);
  }
  gpu.vertices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(all_verts.data()),
               all_verts.size() * sizeof(asset::Vertex)),
      kBufferUsageVertex | rt_usage | ms_usage);
  gpu.indices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(all_indices.data()), all_indices.size() * sizeof(u32)),
      kBufferUsageIndex | rt_usage);
  gpu.index_count = static_cast<u32>(lod.indices.size());    // lod 0 (rt/shadow/overdraw)
  gpu.vertex_count = static_cast<u32>(lod.vertices.size());
  // Skinned meshes carry a parallel bone index/weight stream, bound as a second
  // vertex buffer by the skinned pipeline. Skinned meshes are not lod'd.
  if (mesh.skinned && lod.skinning.size() == lod.vertices.size()) {
    gpu.skinning = device_->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8*>(lod.skinning.data()),
                 lod.skinning.size() * sizeof(asset::SkinnedVertexExtra)),
        kBufferUsageVertex);
    gpu.skinned = static_cast<bool>(gpu.skinning);
  }
  auto build_submeshes = [&](const asset::MeshLod& l, u32 index_base,
                             base::Vector<GpuSubmesh>& out) {
    if (l.submeshes.empty()) {
      out.push_back({index_base, static_cast<u32>(l.indices.size()), 0, false, false});
      return;
    }
    for (const asset::Submesh& submesh : l.submeshes) {
      // Store the salted material hash so every later draw-loop lookup matches
      // this domain's materials (uploaded under the same salt).
      u64 material = submesh.material.hash ^ id_salt;
      bool water = material_system_ && material_system_->is_water(material);
      bool blend = water || (material_system_ && material_system_->is_blend(material));
      bool mask = material_system_ && material_system_->is_mask(material);
      bool effect = material_system_ && material_system_->is_effect(material);
      bool effect_additive = effect && material_system_->is_effect_additive(material);
      GpuSubmesh out_submesh{index_base + submesh.index_offset, submesh.index_count, material,
                             blend, water, mask};
      out_submesh.effect = effect;
      out_submesh.effect_additive = effect_additive;
      out.push_back(out_submesh);
    }
  };
  build_submeshes(src->lods[0], index_bases[0], gpu.submeshes);
  for (size_t i = 1; i < src->lods.size(); ++i) {
    GpuLod glod;
    glod.vertex_offset = vertex_bases[i];
    build_submeshes(src->lods[i], index_bases[i], glod.submeshes);
    gpu.lods.push_back(std::move(glod));
  }
  gpu.all_blend = true;
  for (const GpuSubmesh& submesh : gpu.submeshes) {
    if (!submesh.blend) gpu.all_blend = false;
  }
  std::memcpy(gpu.bounds_center, mesh.bounds_center, sizeof(f32) * 3);
  gpu.bounds_radius = mesh.bounds_radius;
  gpu.no_rt = mesh.exclude_from_rt;
  gpu.terrain_lod = mesh.terrain_lod;

  // Mesh-shader path: split every opaque submesh of every lod into meshlets,
  // concatenated into shared buffers. Each (lod, submesh) records its meshlet
  // range; the task stage picks a lod by distance and dispatches that range.
  // Meshlet vertex indices are rebased to absolute indices into the shared
  // (lod-concatenated) vertex buffer so the mesh shader pulls the right lod.
  if (build_meshlets) {
    base::Vector<Meshlet> all_meshlets;
    base::Vector<u32> all_mv;
    base::Vector<u32> all_mt;
    auto build_lod = [&](base::Vector<GpuSubmesh>& subs, u32 vertex_base, u32 vertex_count) {
      for (GpuSubmesh& submesh : subs) {
        if (submesh.blend || submesh.index_count == 0) continue;
        MeshletGeometry geo = BuildMeshletGeometry(all_verts.data() + vertex_base, vertex_count,
                                                   all_indices.data() + submesh.index_offset,
                                                   submesh.index_count);
        if (geo.meshlets.empty()) continue;
        u32 vtx_base = static_cast<u32>(all_mv.size());
        u32 tri_base = static_cast<u32>(all_mt.size());
        submesh.meshlet_offset = static_cast<u32>(all_meshlets.size());
        submesh.meshlet_count = static_cast<u32>(geo.meshlets.size());
        for (Meshlet m : geo.meshlets) {
          m.vertex_offset += vtx_base;
          m.triangle_offset += tri_base;
          all_meshlets.push_back(m);
        }
        for (u32 v : geo.vertex_indices) all_mv.push_back(v + vertex_base);  // -> global index
        for (u32 t : geo.triangles) all_mt.push_back(t);
      }
    };
    build_lod(gpu.submeshes, vertex_bases[0], static_cast<u32>(src->lods[0].vertices.size()));
    for (size_t i = 1; i < src->lods.size(); ++i) {
      build_lod(gpu.lods[i - 1].submeshes, vertex_bases[i],
                static_cast<u32>(src->lods[i].vertices.size()));
    }
    if (!all_meshlets.empty()) {
      gpu.meshlets = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8*>(all_meshlets.data()),
                   all_meshlets.size() * sizeof(Meshlet)),
          ms_usage);
      gpu.meshlet_vertices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8*>(all_mv.data()), all_mv.size() * sizeof(u32)),
          ms_usage);
      gpu.meshlet_triangles = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8*>(all_mt.data()), all_mt.size() * sizeof(u32)),
          ms_usage);
      gpu.has_meshlets = true;
    }
  }
  // Grass-like fill geometry stays out of the realtime tlas (bloat + noise), but
  // the path tracer wants it (alpha-tested foliage), so include it when path
  // tracing is enabled.
  bool include_rt = !gpu.no_rt || settings_.path_trace;
  if (bindless_ && !gpu.all_blend && include_rt) {
    base::Vector<BindlessRegistry::GeometryRecord> geometries;
    for (const GpuSubmesh& submesh : gpu.submeshes) {
      if (submesh.blend || submesh.index_count == 0) continue;
      geometries.push_back({submesh.index_offset,
                            material_system_->bindless_material(submesh.material)});
    }
    gpu.bindless_index = bindless_->RegisterMesh(gpu.vertices, gpu.indices, geometries.data(),
                                                 static_cast<u32>(geometries.size()));
    if (gpu.bindless_index == BindlessRegistry::kInvalidIndex) gpu.bindless_index = 0;
  }
  // Re-uploading under an existing key (the builtin biped goes up once for
  // the test spawn and again for the npc template) must free the previous
  // buffers or they leak until vkDestroyDevice complains.
  if (GpuMesh* previous = meshes_.find(mesh_key)) {
    device_->WaitIdle();  // uploads happen at load time; never per frame
    device_->DestroyBuffer(previous->vertices);
    device_->DestroyBuffer(previous->indices);
    if (previous->skinning) device_->DestroyBuffer(previous->skinning);
    if (previous->meshlets) device_->DestroyBuffer(previous->meshlets);
    if (previous->meshlet_vertices) device_->DestroyBuffer(previous->meshlet_vertices);
    if (previous->meshlet_triangles) device_->DestroyBuffer(previous->meshlet_triangles);
  }
  meshes_[mesh_key] = gpu;
  // NIF particle emitters ride along with the mesh; every placed draw of it
  // feeds a cpu pool (see emitter_sim_ in BuildFrameGraph).
  if (!mesh.emitters.empty()) register_emitters(mesh.emitters);
  // Pure transparency never enters the tlas: water occluding rtao and
  // shadow rays would black out everything under it.
  if (raytracing_ && !gpu.all_blend && include_rt) raytracing_->BuildBlas(mesh_key, gpu);
  // Foliage uploaded while path tracing was off got no blas/geometry above; flag a
  // catch-up so toggling path tracing on later still pulls it into the tlas
  // (BuildFrameGraph runs EnsureRayTracingGeometry on the next path-traced frame).
  if (raytracing_ && !gpu.all_blend && gpu.no_rt && !include_rt) rt_foliage_dirty_ = true;
  return true;
}

void Renderer::EnsureRayTracingGeometry() {
  if (!bindless_ || !raytracing_ || !material_system_) return;
  for (auto entry : meshes_) {
    GpuMesh& gpu = entry.value;
    // Only the grass-like fill UploadMesh kept out of the tlas; opaque geometry
    // already registered its blas + bindless records at upload time.
    if (gpu.all_blend || !gpu.no_rt) continue;
    if (raytracing_->HasBlas(entry.key)) continue;  // already built
    base::Vector<BindlessRegistry::GeometryRecord> geometries;
    for (const GpuSubmesh& submesh : gpu.submeshes) {
      if (submesh.blend || submesh.index_count == 0) continue;
      geometries.push_back({submesh.index_offset,
                            material_system_->bindless_material(submesh.material)});
    }
    gpu.bindless_index = bindless_->RegisterMesh(gpu.vertices, gpu.indices, geometries.data(),
                                                 static_cast<u32>(geometries.size()));
    if (gpu.bindless_index == BindlessRegistry::kInvalidIndex) gpu.bindless_index = 0;
    raytracing_->BuildBlas(entry.key, gpu);
  }
}

void Renderer::SetDecalAtlas(asset::AssetId texture, asset::AssetId normal_atlas) {
  if (!material_system_) return;
  const GpuImage* img = material_system_->find_texture(texture.hash);
  decal_atlas_view_ = img ? img->view : TextureView{};
  const GpuImage* normal_img =
      normal_atlas ? material_system_->find_texture(normal_atlas.hash) : nullptr;
  decal_normal_atlas_view_ = normal_img ? normal_img->view : TextureView{};
}

bool Renderer::UploadTexture(const asset::Texture& texture, u64 id_salt) {
  if (!material_system_) return false;
  return material_system_->UploadTexture(texture, id_salt);
}

bool Renderer::UploadMaterial(const asset::Material& material, u64 id_salt) {
  if (!material_system_) return false;
  return material_system_->UploadMaterial(material, id_salt);
}

void Renderer::RenderFrame(const FrameView& view) {
  if (!device_ || device_->is_stub() || !swapchain_) return;

  // Wayland surfaces report an undefined currentExtent, so the driver never
  // flags the swapchain out-of-date on a window resize (unlike X11). Poll the
  // window each frame and recreate when it no longer matches the swapchain, so
  // the rendered output tracks the window the same way imgui's DisplaySize does
  // - otherwise the overlay is drawn/hit-tested against a stale size and clicks
  // stop landing on widgets after a resize.
  if (window_) {
    u32 w = window_->width(), h = window_->height();
    if (w != 0 && h != 0 && (w != output_width_ || h != output_height_)) RecreateSwapchain();
  }

  ApplySettings();

  u32 slot = frame_index_ % kFramesInFlight;
  // Waits on the slot's fence, resets its command allocator and transient
  // descriptor pool, and begins recording.
  CommandList* cmd = device_->BeginFrame(slot);

  u32 image_index = 0;
  AcquireResult acquired = swapchain_->Acquire(slot, &image_index);
  if (acquired == AcquireResult::kOutOfDate) {
    RecreateSwapchain();
    return;
  }
  if (acquired != AcquireResult::kOk && acquired != AcquireResult::kSuboptimal) return;

  // Frame generation: acquire a second image for the interpolated present.
  bool fg_frame = false;
  u32 interp_index = 0;
#if defined(RECREATION_HAS_FSR3)
  if (settings_.frame_generation && !settings_.path_trace &&
      swapchain_->color_space() == ColorSpace::kSrgbNonlinear && upscaler_ &&
      upscaler_->kind() == UpscalerKind::kFsr3) {
    if (!framegen_ && !framegen_attempted_) {
      framegen_attempted_ = true;
      framegen_ = CreateFrameGenerator(
          *device_, {.display_width = swapchain_->extent().width,
                     .display_height = swapchain_->extent().height,
                     .render_width = render_width_, .render_height = render_height_});
      if (!framegen_) REC_WARN("framegen: unavailable, presenting real frames only");
    }
    if (framegen_) {
      AcquireResult second = swapchain_->AcquireSecond(slot, &interp_index);
      fg_frame = second == AcquireResult::kOk || second == AcquireResult::kSuboptimal;
    }
  }
#endif

  transient_pool_->BeginFrame();
  graph_.Reset();
  fg_active_frame_ = fg_frame;
  BuildFrameGraph(frames_[slot], image_index, view);
  if (!graph_.Compile(*device_, *transient_pool_)) return;

  profiler_.BeginFrame(*cmd, slot);
  graph_.SetPassHooks(
      [this](CommandList& c, const char* name) { profiler_.BeginPass(c, name); },
      [this](CommandList& c) { profiler_.EndPass(c); });

  PassContext ctx;
  ctx.cmd = cmd;
  ctx.device = device_.get();
  ctx.graph = &graph_;
  // With async passes the graph splits the frame into segments; the returned
  // list is the final one and the only valid argument for SubmitFrame.
  CommandList* final_cmd = graph_.Execute(ctx);

  PresentResult presented;
#if defined(RECREATION_HAS_FSR3)
  Fsr3SharedResources fg_shared;
  if (fg_frame && upscaler_ && upscaler_->fsr3_shared(&fg_shared)) {
    const GpuImage& backbuffer = swapchain_->image(image_index);
    const GpuImage& target = swapchain_->image(interp_index);
    // The graph's final barrier left the backbuffer in PRESENT; bring it back
    // for the interpolation dispatch to sample.
    {
      TextureBarrier to_read =
          Transition(backbuffer, ResourceState::kPresent, ResourceState::kShaderReadCompute);
      final_cmd->TextureBarriers({&to_read, 1});
    }
    FrameGenInputs fin;
    fin.backbuffer = &backbuffer;
    fin.dilated_depth = fg_shared.dilated_depth;
    fin.dilated_motion = fg_shared.dilated_motion;
    fin.recon_prev_depth = fg_shared.recon_prev_depth;
    fin.frame_delta_seconds = view.frame_delta_seconds;
    fin.camera_near = 0.1f;
    fin.camera_fov_y = view.camera.fov_y;
    fin.frame_id = frame_index_;
    fin.reset = !framegen_was_active_;
    bool interpolated = framegen_->Record(*final_cmd, fin);
    framegen_was_active_ = interpolated;

    if (interpolated) {
      const GpuImage& interp = framegen_->interpolated();
      TextureBarrier pre[] = {
          Transition(interp, ResourceState::kGeneral, ResourceState::kCopySrc),
          Transition(target, ResourceState::kUndefined, ResourceState::kCopyDst)};
      final_cmd->TextureBarriers(pre);
      final_cmd->CopyTexture(interp, target);
      TextureBarrier mid[] = {
          Transition(interp, ResourceState::kCopySrc, ResourceState::kGeneral),
          Transition(target, ResourceState::kCopyDst, ResourceState::kColorTarget)};
      final_cmd->TextureBarriers(mid);
      // Re-draw the UI onto the generated frame (the interpolation sourced the
      // pre-UI copy). Both backends replay retained draw data, so recording
      // them twice per frame is safe; blur_source was filled by the ui pass.
      if (view.hud_draw || view.ui_draw) {
        ColorAttachment ui_color{.view = target.view, .load = LoadOp::kLoad};
        final_cmd->BeginRendering({.extent = target.extent, .colors = {&ui_color, 1}});
        if (view.hud_draw) view.hud_draw(*final_cmd);
        if (view.ui_draw) view.ui_draw(*final_cmd);
        final_cmd->EndRendering();
      }
      TextureBarrier post[] = {
          Transition(target, ResourceState::kColorTarget, ResourceState::kPresent),
          Transition(backbuffer, ResourceState::kShaderReadCompute, ResourceState::kPresent)};
      final_cmd->TextureBarriers(post);
    } else {
      // Dispatch failed: duplicate the real frame so the acquired image still
      // presents something sane.
      TextureBarrier pre[] = {
          Transition(backbuffer, ResourceState::kShaderReadCompute, ResourceState::kCopySrc),
          Transition(target, ResourceState::kUndefined, ResourceState::kCopyDst)};
      final_cmd->TextureBarriers(pre);
      final_cmd->CopyTexture(backbuffer, target);
      TextureBarrier post[] = {
          Transition(backbuffer, ResourceState::kCopySrc, ResourceState::kPresent),
          Transition(target, ResourceState::kCopyDst, ResourceState::kPresent)};
      final_cmd->TextureBarriers(post);
    }
    presented = device_->SubmitFrameGen(final_cmd, *swapchain_, interp_index, image_index);
    fg_presents_ += 2;

    // Debug: REC_FRAMEGEN_DUMP=<frame> writes real frame N, the interpolated
    // N->N+1 midpoint and real frame N+1 as pngs in the working directory.
    if (const char* dump = std::getenv("REC_FRAMEGEN_DUMP")) {
      u64 dump_frame = std::strtoull(dump, nullptr, 10);
      if (frame_index_ == dump_frame) {
        DumpFgImage(swapchain_->image(image_index), ResourceState::kPresent, true,
                    "fg_dump_real0.png");
      } else if (frame_index_ == dump_frame + 1) {
        DumpFgImage(framegen_->interpolated(), ResourceState::kGeneral, false,
                    "fg_dump_interp.png");
        DumpFgImage(framegen_->hudless(), ResourceState::kShaderReadCompute, false,
                    "fg_dump_hudless.png");
        DumpFgImage(swapchain_->image(image_index), ResourceState::kPresent, true,
                    "fg_dump_real1.png");
      }
    }
  } else
#endif
  {
    presented = device_->SubmitFrame(final_cmd, *swapchain_, image_index);
    framegen_was_active_ = false;
    fg_presents_ += 1;
  }

  // Present-rate accounting: the observable proof that generation runs.
  ++fg_engine_frames_;
  if (settings_.frame_generation && time_seconds_ - fg_log_time_ >= 2.0) {
    if (fg_log_time_ > 0.0) {
      f64 span = time_seconds_ - fg_log_time_;
      REC_INFO("framegen: {:.0f} engine fps -> {:.0f} presented fps",
               fg_engine_frames_ / span, fg_presents_ / span);
    }
    fg_log_time_ = time_seconds_;
    fg_engine_frames_ = 0;
    fg_presents_ = 0;
  }

  if (!screenshot_path_.empty() && time_seconds_ >= screenshot_at_) {
    WriteScreenshot(image_index);
  }
  if (hdr_pending_) {
    WriteHdr();
    hdr_pending_ = false;
  }

  if (presented == PresentResult::kOutOfDate) {
    RecreateSwapchain();
  }
  ++frame_index_;
}

void Renderer::BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view) {
  u32 frame_slot = frame_index_ % kFramesInFlight;
  bool rt_shadows = rt_available_ && settings_.rt_shadows;
  bool rtao_active = rt_available_ && settings_.rtao;
  bool ddgi_active = ddgi_ && settings_.ddgi && settings_.ibl;
  bool reflections_active = rt_available_ && settings_.rt_reflections && bindless_ != nullptr;
  // The ray-query fragment variant serves both shadows and reflections.
  bool use_rt_frag = rt_shadows || reflections_active;
  bool path_trace = rt_available_ && bindless_ != nullptr && settings_.path_trace;
  // Scene pass consumes last frame's rate image; the rebuild pass below the
  // transparents keeps it fresh. Wireframe wants exact per-pixel lines.
  vrs_active_ = settings_.vrs && vrs_.available() && !path_trace && !settings_.wireframe;
  // Foliage uploaded before path tracing was enabled has no blas (it was excluded
  // from the realtime tlas). Build it now so alpha-tested vegetation appears when
  // path tracing is toggled on at runtime, not only when set before content load.
  if (path_trace && rt_foliage_dirty_) {
    EnsureRayTracingGeometry();
    rt_foliage_dirty_ = false;
  }
  bool fog_active = rt_available_ && settings_.fog && !path_trace;
  // Ambient occlusion technique: ray-traced + NRD-denoised when available, else
  // the screen-space fallback so non-rt tiers (and forced low presets) keep ao.
  bool nrd_ao = false;
  bool nrd_shadow = false;
#if defined(RECREATION_HAS_NRD)
  nrd_ao = rtao_active && nrd_.available();
  nrd_shadow = rt_shadows && nrd_.available();
#endif
  // Denoised stochastic reflections need the NRD specular denoiser; without
  // it the rt fragment variant keeps its inline deterministic mirror ray.
  bool spec_refl_active = false;
#if defined(RECREATION_HAS_NRD)
  spec_refl_active = reflections_active && nrd_.available() && !path_trace;
#endif
  bool ss_ao = settings_.ssao && !nrd_ao && !path_trace;
  // Cascaded shadow maps: the raster sun-shadow path, used whenever ray-traced
  // shadows are not. The rt fragment variant traces its own shadow ray instead.
  bool csm_active = settings_.shadow_maps && !rt_shadows && !path_trace;
  // When the tlas is consulted for shading, the rasterized surface must match the
  // blas (built at lod 0), or ao/reflection rays self-intersect the finer sphere
  // and read black. Distance lod then only applies on non-rt (low/mobile) tiers.
  bool tlas_shaded =
      rt_shadows || rtao_active || ddgi_active || reflections_active || path_trace;
  // Screen-space reflections stand in for ray-traced reflections on raster tiers.
  bool ssr_active = settings_.ssr && !path_trace && !reflections_active;
  // Screen-space gi stands in for the ddgi probe volume on raster tiers.
  bool ssgi_active = settings_.ssgi && !path_trace && !ddgi_active;
  time_seconds_ += view.frame_delta_seconds;

  // Transparent work is gathered up front: water forces a tlas (the water
  // pipeline statically binds it) and an opaque snapshot pass.
  struct TransparentDraw {
    const DrawItem* item;
    const GpuSubmesh* submesh;
    f32 distance_sq;
  };
  base::Vector<TransparentDraw> transparent;
  bool any_water = false;
  for (const DrawItem& item : view.draws) {
    const GpuMesh* mesh = meshes_.find(item.mesh);
    if (!mesh) continue;
    for (const GpuSubmesh& submesh : mesh->submeshes) {
      if (!submesh.blend) continue;
      f32 dx = item.transform.m[12] - view.camera.eye.x;
      f32 dy = item.transform.m[13] - view.camera.eye.y;
      f32 dz = item.transform.m[14] - view.camera.eye.z;
      transparent.push_back({&item, &submesh, dx * dx + dy * dy + dz * dz});
      if (submesh.water) any_water = true;
    }
  }
  bool water_pipeline_active = any_water && water_ != nullptr;

  u32 tlas_slot = frame_index_ % RayTracingContext::kSlots;

  // The frame's globals set (uniform + optional tlas + optional hi-z) is
  // rewritten once per frame, from the first pass that needs it. The slot's
  // fence has fired, so its previous frame no longer reads the set.
  BindingSetHandle globals_set = globals_sets_[frame_slot];
  auto update_globals_set = [this, globals_set, tlas_slot](PassContext& ctx,
                                                           ResourceHandle cull_hiz,
                                                           bool ms_active, bool want_tlas) {
    base::Vector<BindingItem> items;
    items.push_back(Bind::Uniform(0, frames_[frame_index_ % kFramesInFlight].globals, 0,
                                  sizeof(FrameGlobals)));
    if (want_tlas && rt_available_ && raytracing_ && raytracing_->tlas(tlas_slot)) {
      items.push_back(Bind::Accel(1, raytracing_->tlas(tlas_slot)));
    }
    if (ms_active) {  // hi-z for the task-stage occlusion cull (real or fallback)
      TextureView hiz = cull_hiz != kInvalidResource ? ctx.graph->image(cull_hiz).view
                                                     : ms_dummy_hiz_.view;
      items.push_back(Bind::SampledView(2, hiz));
    }
    device_->UpdateBindingSet(globals_set, {items.data(), items.size()});
  };

  // Water + transparency over an opaque base. A lambda (rather than inline) so the
  // path tracer, which otherwise skips the whole raster transparency path, can
  // composite water over its result too. Consumes `transparent` (moved into the
  // pass), so it runs at most once per frame. Returns the composited colour.
  auto add_water = [&](ResourceHandle scene_color, ResourceHandle depth,
                       ResourceHandle depth_export, ResourceHandle motion,
                       ResourceHandle sun_shadow, ResourceHandle shadow_atlas, bool csm_on,
                       u32 shadow_slot, u32 water_tlas_slot,
                       bool globals_written) -> ResourceHandle {
    std::sort(transparent.begin(), transparent.end(),
              [](const TransparentDraw& a, const TransparentDraw& b) {
                return a.distance_sq > b.distance_sq;
              });

    // Transparency renders into a copy of the opaque result and refracts by
    // sampling the original, which never returns to attachment layout
    // afterwards: re-attaching a sampled image corrupts its compression
    // metadata on nvidia (the depth export exists for the same reason).
    ResourceHandle composite = graph_.CreateTexture(
        {.name = "composite", .format = kSceneColorFormat, .width = render_width_,
         .height = render_height_});
    graph_.AddPass(
        "opaque_copy",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Read(scene_color, ResourceUsage::kSampledCompute);
          builder.Write(composite, ResourceUsage::kStorageWrite);
        },
        [this, scene_color, composite](PassContext& ctx) {
          water_->RecordCopy(ctx, scene_color, composite, render_width_, render_height_);
        });

    ResourceHandle opaque_color = scene_color;
    ResourceHandle opaque_depth = depth_export;
    graph_.AddPass(
        "transparent",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Write(composite, ResourceUsage::kColorAttachment);
          builder.Write(motion, ResourceUsage::kColorAttachment);
          builder.Write(depth, ResourceUsage::kDepthAttachment);
          builder.Read(opaque_color, ResourceUsage::kSampledFragment);
          builder.Read(opaque_depth, ResourceUsage::kSampledFragment);
          if (sun_shadow != kInvalidResource)
            builder.Read(sun_shadow, ResourceUsage::kSampledFragment);
          if (csm_on) builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
        },
        [this, composite, motion, depth, opaque_color, opaque_depth, sun_shadow, water_tlas_slot,
         use_rt_frag, ddgi_active, water_pipeline_active, csm_on, shadow_slot, shadow_atlas,
         globals_set, globals_written, update_globals_set, frame_slot,
         transparent = std::move(transparent), &frame, &view](PassContext& ctx) {
          if (!globals_written) {
            update_globals_set(ctx, kInvalidResource, false, /*want_tlas=*/true);
          }

          BindingSetHandle env_set = env_transparent_sets_[frame_slot];
          EnvironmentSystem::DdgiBinding ddgi_binding;
          if (ddgi_active) ddgi_binding = ddgi_->binding(frame_index_);
          TextureView sun_shadow_view =
              sun_shadow != kInvalidResource ? ctx.graph->image(sun_shadow).view : TextureView{};
          environment_->WriteEnvSet(
              env_set, TextureView{}, ddgi_active ? &ddgi_binding : nullptr,
              csm_on ? ctx.graph->image(shadow_atlas).view : TextureView{},
              csm_on ? shadow_.cascade_buffer(shadow_slot) : GpuBuffer{},
              shadow_.cascade_buffer_size(), ctx.graph->image(opaque_color).view, sun_shadow_view,
              frame.lights, frame.lights.size, TextureView{}, cluster_counts_, cluster_indices_,
              frame.decals, decal_cluster_indices_, decal_atlas_view_,
              local_shadows_active_ ? local_shadows_.face_buffer(frame_slot) : GpuBuffer{},
              local_shadows_active_ ? local_shadows_.atlas().view : TextureView{},
              decal_normal_atlas_view_, TextureView{}, TextureView{}, GpuBuffer{}, TextureView{},
              TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{});

          ColorAttachment colors[2];
          colors[0] = {.view = ctx.graph->image(composite).view, .load = LoadOp::kLoad};
          colors[1] = {.view = ctx.graph->image(motion).view, .load = LoadOp::kLoad};
          DepthAttachment depth_attachment{.view = ctx.graph->image(depth).view,
                                           .load = LoadOp::kLoad};
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = {colors, 2},
                                   .depth = &depth_attachment});

          // Effect-shader fire/glows use the additive blend pipeline; the same
          // unlit fragment shader serves both the alpha and additive effect
          // materials (it premultiplies coverage for the additive one).
          enum class Mode { kNone, kWater, kBlend, kBlendAdditive };
          Mode mode = Mode::kNone;
          BindingSetHandle bound_material{};
          const DrawItem* bound_item = nullptr;
          for (const TransparentDraw& draw : transparent) {
            const GpuMesh* mesh = meshes_.find(draw.item->mesh);
            if (!mesh) continue;
            bool as_water = draw.submesh->water && water_pipeline_active;
            bool additive = draw.submesh->effect_additive;

            Mode wanted = as_water ? Mode::kWater
                                   : (additive ? Mode::kBlendAdditive : Mode::kBlend);
            if (mode != wanted) {
              BindingSetHandle bindless_set = bindless_ ? bindless_->set() : BindingSetHandle{};
              if (as_water) {
                water_->Bind(ctx, globals_set, env_set, bindless_->set(), opaque_color,
                             opaque_depth);
              } else if (additive) {
                mesh_pipeline_->BindBlendAdditive(*ctx.cmd, globals_set, env_set, bindless_set,
                                                  use_rt_frag);
              } else {
                mesh_pipeline_->BindBlend(*ctx.cmd, globals_set, env_set, bindless_set,
                                          use_rt_frag);
              }
              mode = wanted;
              bound_material = {};
              bound_item = nullptr;
            }
            if (draw.item != bound_item) {
              MeshPushConstants push{.model = draw.item->transform,
                                     .prev_model = draw.item->prev_transform};
              if (as_water) {
                // The water pipeline shares the mesh push block. Push the whole
                // struct so detail_rect is zeroed: mesh.vs sinks vertices inside
                // a nonzero rect, and a stale one would sink the water plane.
                ctx.cmd->PushConstants(&push, sizeof(push));
                ctx.cmd->BindVertexBuffer(0, mesh->vertices);
                ctx.cmd->BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
              } else {
                mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
              }
              bound_item = draw.item;
            }
            BindingSetHandle material = material_system_->set(draw.submesh->material);
            if (!(material == bound_material)) {
              if (as_water) {
                water_->BindMaterial(*ctx.cmd, material);
              } else {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
              }
              bound_material = material;
            }
            mesh_pipeline_->DrawSubmesh(*ctx.cmd, *draw.submesh);
          }
          ctx.cmd->EndRendering();
        });
    return composite;
  };

  // Camera state for both this frame and reprojection. Jitter lives in the
  // projection, not the matrices used for motion vectors.
  f32 aspect = static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
  Mat4 proj = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f);
  Mat4 view_mat = LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
  Mat4 view_proj = proj * view_mat;

  bool temporal =
      settings_.aa_mode == AntiAliasingMode::kTaa ||
      settings_.aa_mode == AntiAliasingMode::kUpscaler;
  f32 jitter_x = 0, jitter_y = 0;
  if (temporal) {
    u32 sample_count = taa_.settings().jitter_sample_count;
    if (settings_.aa_mode == AntiAliasingMode::kUpscaler) {
      // FSR-style phase count grows with the scale factor squared.
      f32 scale = static_cast<f32>(output_width_) / static_cast<f32>(render_width_);
      sample_count = static_cast<u32>(std::ceil(8.0f * scale * scale));
    }
    JitterSequence::Sample(frame_index_, sample_count, &jitter_x, &jitter_y);
  }

  bool first_frame = !has_prev_frame_;

  FrameGlobals globals;
  globals.view_proj = view_proj;
  globals.prev_view_proj = has_prev_frame_ ? prev_view_proj_ : view_proj;
  globals.inv_view_proj = Inverse(view_proj);
  globals.jitter[0] = 2.0f * jitter_x / static_cast<f32>(render_width_);
  globals.jitter[1] = 2.0f * jitter_y / static_cast<f32>(render_height_);
  // Interior cells author their own lighting (XCLL/LGTM): the directional fill
  // rides the sun path with the authored colour/direction, and the sky-derived
  // sun/atmosphere/IBL are suppressed below.
  const bool interior = settings_.interior;
  Vec3 sun = Normalize(interior ? settings_.interior_directional_dir : settings_.sun_direction);
  globals.sun_direction[0] = sun.x;
  globals.sun_direction[1] = sun.y;
  globals.sun_direction[2] = sun.z;
  // Lightning flashes the PER-FRAME direct light only (not settings_, so the
  // sun-change check never rebuilds the IBL cubemap): a brief bright blue-white
  // boost to the directional intensity, colour and ambient fill.
  const f32 flash = interior ? 0.0f : settings_.lightning;
  if (interior) {
    globals.sun_direction[3] = settings_.interior_directional_intensity;
    globals.sun_color[0] = settings_.interior_directional_color.x;
    globals.sun_color[1] = settings_.interior_directional_color.y;
    globals.sun_color[2] = settings_.interior_directional_color.z;
    globals.sun_color[3] = 0.0f;
    globals.interior_ambient[0] = settings_.interior_ambient.x;
    globals.interior_ambient[1] = settings_.interior_ambient.y;
    globals.interior_ambient[2] = settings_.interior_ambient.z;
    globals.interior_fog_color0[0] = settings_.interior_fog_near_color.x;
    globals.interior_fog_color0[1] = settings_.interior_fog_near_color.y;
    globals.interior_fog_color0[2] = settings_.interior_fog_near_color.z;
    globals.interior_fog_color0[3] = settings_.interior_fog_near;
    globals.interior_fog_color1[0] = settings_.interior_fog_far_color.x;
    globals.interior_fog_color1[1] = settings_.interior_fog_far_color.y;
    globals.interior_fog_color1[2] = settings_.interior_fog_far_color.z;
    globals.interior_fog_color1[3] = settings_.interior_fog_far;
    globals.interior_fog_params[0] = settings_.interior_fog_power;
    globals.interior_fog_params[1] = settings_.interior_fog_max;
  } else {
    globals.sun_direction[3] = settings_.sun_intensity + flash * 9.0f;
    globals.sun_color[0] = settings_.sun_color.x + flash * (0.90f - settings_.sun_color.x);
    globals.sun_color[1] = settings_.sun_color.y + flash * (0.95f - settings_.sun_color.y);
    globals.sun_color[2] = settings_.sun_color.z + flash * (1.10f - settings_.sun_color.z);
    globals.sun_color[3] = settings_.ambient + flash * 0.5f;
  }
  globals.camera_position[0] = view.camera.eye.x;
  globals.camera_position[1] = view.camera.eye.y;
  globals.camera_position[2] = view.camera.eye.z;
  globals.camera_position[3] = settings_.ibl_intensity;
  globals.misc[0] = static_cast<f32>(render_width_);
  globals.misc[1] = static_cast<f32>(render_height_);
  globals.misc[2] = settings_.sun_angular_radius;
  globals.misc[3] = static_cast<f32>(frame_index_ % 4096);
  if (settings_.ibl && !interior) globals.flags |= kFrameFlagIbl;
  if (nrd_ao || ss_ao) globals.flags |= kFrameFlagAoValid;
  if (csm_active && !interior) globals.flags |= kFrameFlagShadowMap;
  if (ddgi_active && !interior) globals.flags |= kFrameFlagDdgi;
  if (water_pipeline_active && settings_.water_reflections) globals.flags |= kFrameFlagWaterRt;
  if (rt_shadows && !interior) globals.flags |= kFrameFlagRtShadows;
  if (settings_.aurora && !interior) globals.flags |= kFrameFlagAurora;
  if (nrd_shadow && !interior) globals.flags |= kFrameFlagSigmaShadow;
  if (interior) globals.flags |= kFrameFlagInterior;
  if (reflections_active) globals.flags |= kFrameFlagReflections;
  if (spec_refl_active) globals.flags |= kFrameFlagSpecReflTex;
  globals.time = static_cast<f32>(time_seconds_);
  globals.debug_view = static_cast<u32>(settings_.debug_view);
  globals.reflection_cutoff = settings_.reflection_roughness_cutoff;
  globals.ao_ray_count = nrd_ao ? settings_.ao_rays : 0u;  // rt ao rays, for the ray-count view

  // Dynamic point lights: copy into the host-visible frame buffer (capped).
  u32 light_count = std::min<u32>(static_cast<u32>(view.lights.size()), kMaxFrameLights);
  if (light_count > 0) {
    std::memcpy(frame.lights.mapped, view.lights.data(), light_count * sizeof(PointLight));
  }
  // Local light shadows: the nearest casters claim atlas faces (writes each
  // claimed light's params.w in the mapped buffer, so it must follow the copy).
  local_shadows_active_ = false;
  if (settings_.local_shadows && !path_trace && light_count > 0) {
    local_shadows_.Assign(static_cast<PointLight*>(frame.lights.mapped), light_count,
                          view.camera.eye, frame_slot);
    local_shadows_active_ = local_shadows_.face_count() > 0;
  }
  u32 decal_count = std::min<u32>(static_cast<u32>(view.decals.size()), kMaxFrameDecals);
  if (decal_count > 0) {
    std::memcpy(frame.decals.mapped, view.decals.data(), decal_count * sizeof(Decal));
  }
  globals.light_count = light_count;
  fft_ocean_active_ = settings_.fft_ocean && ocean_.available() && !path_trace && !interior;
  const bool fft_ocean_active = fft_ocean_active_;
  if (fft_ocean_active) globals.flags |= kFrameFlagFftOcean;
  // Hybrid ReSTIR DI decision happens before the globals upload below; the
  // graph passes record later under the same flag.
  bool restir_active = settings_.restir_di && rt_available_ && restir_di_.available() &&
                       !path_trace && light_count > 0 && raytracing_ &&
                       raytracing_->tlas(tlas_slot);
  if (restir_active) globals.flags |= kFrameFlagRestirDi;
  {
    // Froxel slicing: exponential view-z between the near plane and 500 m.
    constexpr f32 kNear = 0.1f, kFar = 500.0f;
    f32 scale = static_cast<f32>(kClusterSlices) / std::log2(kFar / kNear);
    globals.cluster_params[0] = scale;
    globals.cluster_params[1] = -std::log2(kNear) * scale;
    globals.cluster_params[2] =
        static_cast<f32>(render_width_) / static_cast<f32>(kClusterTilesX);
    globals.cluster_params[3] =
        static_cast<f32>(render_height_) / static_cast<f32>(kClusterTilesY);
  }
  std::memcpy(frame.globals.mapped, &globals, sizeof(globals));
  prev_view_proj_ = view_proj;
  has_prev_frame_ = true;

  // Skinning palette for every skinned draw this frame, read by device address.
  if (!view.bone_matrices.empty() && frame.bone_palette.mapped) {
    u32 count = std::min<u32>(static_cast<u32>(view.bone_matrices.size()), kMaxFrameBones);
    std::memcpy(frame.bone_palette.mapped, view.bone_matrices.data(), count * sizeof(Mat4));
  }

  ResourceHandle scene_color = graph_.CreateTexture(
      {.name = "scene_color", .format = kSceneColorFormat, .width = render_width_,
       .height = render_height_});
  // The g-buffer aux targets only exist for the raster path; the path tracer
  // writes scene_color directly, so leaving them uncreated keeps the transient
  // pool from allocating images no pass touches.
  ResourceHandle motion = kInvalidResource, depth = kInvalidResource;
  ResourceHandle normals = kInvalidResource, depth_export = kInvalidResource;
  ResourceHandle shadow_atlas = kInvalidResource;
  if (csm_active) {
    shadow_atlas = graph_.CreateTexture({.name = "shadow_atlas",
                                         .format = Format::kD32Float,
                                         .width = shadow_.atlas_width(),
                                         .height = shadow_.atlas_height()});
  }
  if (!path_trace) {
    motion = graph_.CreateTexture(
        {.name = "motion", .format = kMotionFormat, .width = render_width_,
         .height = render_height_});
    depth = graph_.CreateTexture(
        {.name = "depth", .format = kDepthFormat, .width = render_width_,
         .height = render_height_});
    normals = graph_.CreateTexture(
        {.name = "normals", .format = kNormalFormat, .width = render_width_,
         .height = render_height_});
    // Raw reversed z exported by the prepass; every depth consumer samples
    // this so the real depth attachment never changes layout mid frame
    // (sampling round trips corrupt its compression metadata on nvidia).
    depth_export = graph_.CreateTexture(
        {.name = "depth_export", .format = Format::kR32Float, .width = render_width_,
         .height = render_height_});
  }
  // Skin diffuse export: third scene-pass attachment, diffused by the
  // screen-space sss blur after the opaque pass (raster path only).
  ResourceHandle skin_diffuse = kInvalidResource;
  if (!path_trace) {
    skin_diffuse = graph_.CreateTexture(
        {.name = "skin_diffuse", .format = MeshPipeline::kSkinDiffuseFormat,
         .width = render_width_, .height = render_height_});
  }

  // Virtual texturing: drain last frame's page requests, stream pages, and
  // record this frame's atlas/indirection uploads + the feedback copy/reset.
  if (!path_trace) virtual_texture_.AddToGraph(graph_, frame_index_);

  // FFT ocean: evolve the spectrum and rebuild the displacement/normal maps
  // the water shaders sample this frame.
  if (fft_ocean_active) ocean_.AddToGraph(graph_, static_cast<f32>(time_seconds_));

  if (environment_dirty_ && (settings_.ibl || settings_.sky)) {
    environment_dirty_ = false;
    Vec3 env_sun = applied_sun_direction_;
    f32 env_intensity = applied_sun_intensity_;
    Vec3 env_color = applied_sun_color_;
    graph_.AddPass(
        "env_update", [](RenderGraph::PassBuilder&) {},
        [this, env_sun, env_intensity, env_color](PassContext& ctx) {
          environment_->RecordUpdate(*ctx.cmd, env_sun, env_intensity, env_color);
        });
  }

  if (rt_shadows || rtao_active || ddgi_active || water_pipeline_active || reflections_active ||
      path_trace || fog_active) {
    base::Vector<RayTracingContext::Instance> instances;
    instances.reserve(view.draws.size());
    for (const DrawItem& item : view.draws) {
      const GpuMesh* mesh = meshes_.find(item.mesh);
      // no_rt grass-like fill is excluded from the realtime tlas, but the path
      // tracer wants it (it built a blas for it; see UploadMesh include_rt).
      if (!mesh || mesh->all_blend || (mesh->no_rt && !path_trace)) continue;
      instances.push_back({.mesh_key = item.mesh,
                           .custom_index = mesh->bindless_index,
                           .transform = item.transform});
    }
    // Grow the TLAS now, on the build thread, so the record-time BuildTlas never
    // stalls the device or frees buffers mid command list (which races the
    // frame ring and corrupts the image). Spikes here when two worlds stream in.
    raytracing_->ReserveTlas(tlas_slot, static_cast<u32>(instances.size()));
    graph_.AddPass(
        "tlas_build", [](RenderGraph::PassBuilder&) {},
        [this, tlas_slot, instances = std::move(instances)](PassContext& ctx) {
          raytracing_->BuildTlas(*ctx.cmd, tlas_slot, instances);
        });
  }

  // DDGI right after the TLAS (its only same-frame dependency): flagged async
  // it forks onto the compute queue here and overlaps everything up to the
  // join before its first consumer (the reflection trace / scene pass).
  bool ddgi_async = ddgi_active && !path_trace && settings_.async_compute &&
                    device_->caps().async_compute;
  if (ddgi_active && !path_trace) {
    ddgi_->AddToGraph(graph_, *raytracing_, tlas_slot, view.camera.eye,
                      applied_sun_direction_, applied_sun_intensity_, applied_sun_color_,
                      frame_index_, ddgi_async);
  }

  // The path tracer takes over the whole frame: it writes scene_color directly
  // and skips the entire raster path (g-buffer, gi, transparency, aa).
  ResourceHandle lit = scene_color;
  if (path_trace) {
    PathTracer::Frame pt;
    pt.inv_view_proj = globals.inv_view_proj;
    pt.view_proj = view_proj;
    pt.prev_view_proj = globals.prev_view_proj;  // last frame's, set before the overwrite below
    pt.camera_pos = view.camera.eye;
    pt.sun_direction = settings_.sun_direction;
    pt.sun_intensity = settings_.sun_intensity;
    pt.sun_color = settings_.sun_color;
    pt.sun_radius = settings_.sun_angular_radius;
    pt.frame_index = frame_index_;
    f32 sig = settings_.sun_intensity + settings_.sun_color.x * 3.0f +
              settings_.sun_color.y * 5.0f + settings_.sun_color.z * 7.0f;
    bool moved = std::memcmp(&view_proj, &pt_prev_view_proj_, sizeof(Mat4)) != 0;
    bool lit_changed = sig != pt_prev_sig_;
    bool denoised_path = false;

    // Gameplay reconstruction renderer: own 1-spp gbuffer + temporal accumulation
    // + a-trous denoise + composite. Separate from the brute-force reference and
    // the NRD path. Reference always wins (screenshots); else recon if selected.
    bool recon_path = settings_.path_trace_recon && !settings_.path_trace_reference;
    if (recon_path) {
      // Lazily allocate the recon history targets on first use: the mode is off by
      // default and the buffers are large, so they are not created up front.
      recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
      bool rr_active = false;
#if defined(RECREATION_HAS_DLSS)
      // Ray reconstruction replaces the SVGF chain when its snippet loads;
      // lazy-init mirrors the recon targets above.
      if (settings_.path_trace_rr && !rr_init_attempted_) {
        rr_init_attempted_ = true;
        if (!rr_.Initialize(*device_, {render_width_, render_height_})) {
          REC_INFO("dlss-rr unavailable, recon uses the in-tree svgf denoiser");
        }
      }
      rr_active = settings_.path_trace_rr && rr_.available();
#endif
      ReconPathTracer::Frame rf;
      rf.inv_view_proj = globals.inv_view_proj;
      rf.view_proj = view_proj;
      rf.prev_view_proj = globals.prev_view_proj;
      rf.camera_pos = view.camera.eye;
      rf.sun_direction = settings_.sun_direction;
      rf.sun_intensity = settings_.sun_intensity;
      rf.sun_color = settings_.sun_color;
      rf.sun_radius = settings_.sun_angular_radius;
      rf.pixel_spread = 2.0f * std::tan(view.camera.fov_y * 0.5f) / static_cast<f32>(render_height_);
      rf.spp = settings_.path_trace_spp;
      rf.frame_index = frame_index_;
      // Reset on first frame, on (re)activation, AND when switching into recon
      // from another path-trace mode (its ping-pong history was never written by
      // the reference/NRD paths). Never on the day/night drift.
      rf.reset = first_frame || !pt_was_active_ || pt_prev_mode_ != 2;
      rf.current_weight_min = settings_.path_trace_recon_weight;
      rf.max_history = settings_.path_trace_accum;
      rf.atrous_passes = settings_.path_trace_recon_atrous;
      rf.debug_mode = settings_.path_trace_recon_debug;
      // Modes 8/9 visualize the restir reservoir (M / W): the spatial pass
      // substitutes the heatmap, the composite renders it as raw lighting.
      if (rf.debug_mode >= 8) {
        rf.restir = true;
      }
      rf.restir = settings_.path_trace_restir;
      rf.restir_di = settings_.path_trace_restir_di;
      rf.lights = frame.lights;
      rf.light_count = light_count;
      rf.fog = settings_.fog;
      rf.fog_density = settings_.fog_density;
      rf.fog_height_falloff = settings_.fog_height_falloff;
      rf.fog_base_height = settings_.fog_base_height;
      rf.fog_anisotropy = settings_.fog_anisotropy;
#if defined(RECREATION_HAS_DLSS)
      if (rr_active) {
        ReconPathTracer::ExternalInputs ext;
        recon_path_tracer_.AddToGraph(graph_, *raytracing_, tlas_slot, bindless_->set(),
                                      environment_->sky_view(), environment_->sampler(),
                                      scene_color, rf, &ext);
        RrDenoiser::Frame rrf;
        rrf.world_to_view = view_mat;
        rrf.view_to_clip = proj;
        rrf.frame_delta_ms = view.frame_delta_seconds * 1000.0f;
        rrf.reset = rf.reset;
        rr_.AddToGraph(graph_,
                       {ext.color, ext.depth, ext.motion, ext.normals_rough, ext.diffuse_albedo,
                        ext.specular_albedo},
                       scene_color, rrf);
      } else
#endif
      {
        (void)rr_active;
        recon_path_tracer_.AddToGraph(graph_, *raytracing_, tlas_slot, bindless_->set(),
                                      environment_->sky_view(), environment_->sampler(),
                                      scene_color, rf);
      }
    }
#if defined(RECREATION_HAS_NRD)
    if (!recon_path && nrd_.available() && !settings_.path_trace_reference) {
      // Playable: spp lighting samples, then NRD's REBLUR_DIFFUSE reprojects
      // history across camera motion (no full reset), so the view stays clean
      // while moving. More spp = lower input variance = less shimmer.
      denoised_path = true;
      pt.spp = settings_.path_trace_spp;
      // Ray-cone spread for texture lod: vertical fov radians per pixel.
      pt.pixel_spread = 2.0f * std::tan(view.camera.fov_y * 0.5f) / static_cast<f32>(render_height_);
      PathTracer::GbufferTargets t;
      auto guide = [&](const char* name, Format format) {
        return graph_.CreateTexture({.name = name, .format = format, .width = render_width_,
                                     .height = render_height_});
      };
      t.radiance_hitdist = guide("pt_radiance", NrdDenoiser::kDiffuseRadianceFormat);
      t.normal_roughness = guide("pt_normal_roughness", NrdDenoiser::kNormalRoughnessFormat);
      t.viewz = guide("pt_viewz", NrdDenoiser::kViewZFormat);
      t.motion = guide("pt_motion", kMotionFormat);
      t.albedo = guide("pt_albedo", kSceneColorFormat);
      t.background = guide("pt_background", kSceneColorFormat);
      path_tracer_.AddGbufferPass(graph_, *raytracing_, tlas_slot, bindless_->set(),
                                  environment_->sky_view(), environment_->sampler(), t, pt);

      NrdDenoiser::FrameSettings fs;
      fs.view_to_clip = proj;
      fs.view_to_clip_prev = prev_proj_;
      fs.world_to_view = view_mat;
      fs.world_to_view_prev = prev_view_;
      fs.jitter[0] = fs.jitter[1] = 0.0f;  // the path tracer shoots un-jittered rays
      fs.jitter_prev[0] = fs.jitter_prev[1] = 0.0f;
      fs.sun_direction = sun;
      fs.frame_index = frame_index_;
      fs.diffuse_accumulated_frames = settings_.path_trace_accum;
      // Restart ONLY on activation / first frame. NOT on lighting change: the
      // day/night cycle nudges the sun every frame, so resetting on that would
      // restart accumulation every frame and the image would never denoise (it
      // would stay 1-spp grainy forever). NRD tracks gradual lighting changes
      // through its own temporal accumulation + antilag instead. Also reset when
      // switching into the NRD path from another mode (stale reprojection history).
      fs.reset = first_frame || !pt_was_active_ || pt_prev_mode_ != 1;
      nrd_.SetFrame(fs);
      ResourceHandle denoised = nrd_.DenoiseDiffuse(graph_, t.normal_roughness, t.viewz, t.motion,
                                                    t.radiance_hitdist);
      path_tracer_.AddCompositePass(graph_, denoised, t.albedo, t.background, scene_color);

      // The raster path stores these for NRD only when it runs; keep them current
      // for the next path-traced frame's motion vectors and reprojection.
      prev_proj_ = proj;
      prev_view_ = view_mat;
      prev_jitter_[0] = prev_jitter_[1] = 0.0f;
    }
#endif
    if (!denoised_path && !recon_path) {
      // Reference: brute-force accumulation, hard reset on any motion = ground
      // truth. Also reset when switching into reference from another mode.
      pt.reset = !pt_was_active_ || moved || lit_changed || pt_prev_mode_ != 0;
      path_tracer_.AddToGraph(graph_, *raytracing_, tlas_slot, bindless_->set(),
                              environment_->sky_view(), environment_->sampler(), scene_color, pt);
    }
    pt_prev_view_proj_ = view_proj;
    pt_prev_sig_ = sig;
    pt_was_active_ = true;
    pt_prev_mode_ = recon_path ? 2 : (denoised_path ? 1 : 0);
  } else {
    pt_was_active_ = false;
    pt_prev_mode_ = -1;

  u32 shadow_slot = frame_index_ % 2;
  if (csm_active) {
    Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
    Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
    Vec3 up = Cross(right, fwd);
    f32 shadow_aspect = static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
    shadow_.Update(view.camera.eye, fwd, right, up, view.camera.fov_y, shadow_aspect,
                   settings_.sun_direction, shadow_slot);
    graph_.AddPass(
        "shadow_cascades",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Write(shadow_atlas, ResourceUsage::kDepthAttachment);
        },
        [this, shadow_atlas, &frame, &view](PassContext& ctx) {
          TextureView atlas = ctx.graph->image(shadow_atlas).view;
          shadow_.Render(*ctx.cmd, atlas, [this, &frame, &view](CommandList& cmd) {
            BindingSetHandle bound_material{};
            PipelineHandle bound_pipeline{};
            for (const DrawItem& item : view.draws) {
              const GpuMesh* mesh = meshes_.find(item.mesh);
              // no_rt skips grass-like fill geometry, but skinned actors are
              // no_rt only to stay out of the tlas; they still cast shadows.
              if (!mesh || mesh->all_blend || (mesh->no_rt && !mesh->skinned)) continue;
              // Skinned casters run the bone-blended vertex stage so the
              // shadow tracks the animated pose, not the bind pose.
              bool draw_skinned = mesh->skinned && item.skin_offset >= 0 &&
                                  static_cast<bool>(shadow_.skinned_pipeline());
              PipelineHandle pipeline =
                  draw_skinned ? shadow_.skinned_pipeline() : shadow_.pipeline();
              if (!(pipeline == bound_pipeline)) {
                cmd.BindPipeline(pipeline);
                bound_pipeline = pipeline;
              }
              // The per-cascade light matrix sits at offset 0 (pushed by
              // ShadowPass::Render); the model follows it, skin data after.
              cmd.PushConstants(&item.transform, sizeof(Mat4), sizeof(Mat4));
              cmd.BindVertexBuffer(0, mesh->vertices);
              if (draw_skinned) {
                cmd.BindVertexBuffer(1, mesh->skinning);
                struct {
                  u64 bone_address;
                  u32 skin_offset;
                  u32 pad;
                } skin{frame.bone_palette.address, static_cast<u32>(item.skin_offset), 0};
                cmd.PushConstants(&skin, sizeof(skin), 2 * sizeof(Mat4));
              }
              cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
              for (const GpuSubmesh& submesh : mesh->submeshes) {
                if (submesh.blend) continue;
                // Bind the material so masked casters alpha-test in the fragment.
                BindingSetHandle material = material_system_->set(submesh.material);
                if (!(material == bound_material)) {
                  cmd.BindSet(0, material);
                  bound_material = material;
                }
                cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
              }
            }
          });
        });
  }

  // Local light shadow faces: depth-only renders with a light-radius culled
  // draw list, into the persistent atlas the cluster loop samples.
  if (local_shadows_active_) {
    graph_.AddPass(
        "local_shadows", [](RenderGraph::PassBuilder&) {},
        [this, &frame, &view](PassContext& ctx) {
          local_shadows_.Render(
              *ctx.cmd, shadow_.pipeline(),
              [this, &frame, &view](CommandList& cmd, const LocalShadows::Face& face) {
                BindingSetHandle bound_material{};
                PipelineHandle bound_pipeline{};
                for (const DrawItem& item : view.draws) {
                  const GpuMesh* mesh = meshes_.find(item.mesh);
                  if (!mesh || mesh->all_blend || (mesh->no_rt && !mesh->skinned)) continue;
                  // Sphere cull against the light's influence: local shadows only
                  // ever see casters inside the radius.
                  Vec3 wc = TransformPoint(item.transform,
                                           {mesh->bounds_center[0], mesh->bounds_center[1],
                                            mesh->bounds_center[2]});
                  const f32* m = item.transform.m;
                  f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                  f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
                  f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
                  f32 wr = mesh->bounds_radius * std::max(sx, std::max(sy, sz));
                  Vec3 d{wc.x - face.light_pos.x, wc.y - face.light_pos.y,
                         wc.z - face.light_pos.z};
                  f32 reach = face.light_radius + wr;
                  if (wr > 0.0f && d.x * d.x + d.y * d.y + d.z * d.z > reach * reach) continue;

                  bool draw_skinned = mesh->skinned && item.skin_offset >= 0 &&
                                      static_cast<bool>(shadow_.skinned_pipeline());
                  PipelineHandle pipeline =
                      draw_skinned ? shadow_.skinned_pipeline() : shadow_.pipeline();
                  if (!(pipeline == bound_pipeline)) {
                    cmd.BindPipeline(pipeline);
                    bound_pipeline = pipeline;
                  }
                  cmd.PushConstants(&item.transform, sizeof(Mat4), sizeof(Mat4));
                  cmd.BindVertexBuffer(0, mesh->vertices);
                  if (draw_skinned) {
                    cmd.BindVertexBuffer(1, mesh->skinning);
                    struct {
                      u64 bone_address;
                      u32 skin_offset;
                      u32 pad;
                    } skin{frame.bone_palette.address, static_cast<u32>(item.skin_offset), 0};
                    cmd.PushConstants(&skin, sizeof(skin), 2 * sizeof(Mat4));
                  }
                  cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
                  for (const GpuSubmesh& submesh : mesh->submeshes) {
                    if (submesh.blend) continue;
                    BindingSetHandle material = material_system_->set(submesh.material);
                    if (!(material == bound_material)) {
                      cmd.BindSet(0, material);
                      bound_material = material;
                    }
                    cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
                  }
                }
              });
        });
  }

  // GPU-driven culling: build one indirect command per opaque submesh and one
  // cull instance per opaque mesh, in the exact order the prepass/scene draw
  // loops walk view.draws, then let a compute pass zero the culled instanceCounts.
  u32 cull_slot = frame_index_ % 2;
  gpu_cull_.ResizeDepth(*device_, render_width_, render_height_);
  const GpuBuffer& cull_commands = gpu_cull_.command_buffer(cull_slot);
  u32 cull_instance_count = 0;
  {
    GpuCull::Instance* insts = gpu_cull_.instances(cull_slot);
    GpuCull::Command* cmds = gpu_cull_.commands(cull_slot);
    u32 cmd_total = 0;
    for (const DrawItem& item : view.draws) {
      const GpuMesh* mesh = meshes_.find(item.mesh);
      if (!mesh || mesh->all_blend) continue;
      if (cull_instance_count >= GpuCull::kMaxInstances || cmd_total >= GpuCull::kMaxCommands) break;
      GpuCull::Instance& inst = insts[cull_instance_count];
      inst.model = item.transform;
      inst.bounds[0] = mesh->bounds_center[0];
      inst.bounds[1] = mesh->bounds_center[1];
      inst.bounds[2] = mesh->bounds_center[2];
      inst.bounds[3] = mesh->bounds_radius;
      inst.first_cmd = cmd_total;
      inst.cull_disabled = (mesh->skinned || mesh->bounds_radius <= 0.0f) ? 1u : 0u;
      inst.pad = 0;

      // Default to lod 0 (finest): we stream and render the highest detail the
      // game ships. Distance-based downgrade is opt-in (settings_.distance_lod).
      // Skinned meshes and rt-shaded meshes always stay on lod 0 (their bounds
      // deform / the tlas is built from lod 0). The submesh count matches lod 0
      // so the prepass/scene draw loops issue one indirect per submesh.
      bool fixed_lod = !settings_.distance_lod || mesh->skinned || (tlas_shaded && !mesh->no_rt);
      u32 lod = 0;
      if (!fixed_lod) {
        Vec3 wc = TransformPoint(item.transform, {mesh->bounds_center[0], mesh->bounds_center[1],
                                                  mesh->bounds_center[2]});
        Vec3 d = view.camera.eye - wc;
        lod = SelectLod(*mesh, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
      }
      const base::Vector<GpuSubmesh>& lod_subs =
          lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
      i32 vtx_off = lod == 0 ? 0 : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);

      u32 mesh_cmds = 0;
      u32 k = 0;
      for (const GpuSubmesh& submesh : mesh->submeshes) {
        if (!submesh.blend) {
          if (cmd_total >= GpuCull::kMaxCommands) break;
          const GpuSubmesh& s = k < lod_subs.size() ? lod_subs[k] : submesh;
          cmds[cmd_total] = {s.index_count, 1u, s.index_offset, vtx_off, 0u};
          ++cmd_total;
          ++mesh_cmds;
        }
        ++k;
      }
      inst.cmd_count = mesh_cmds;
      if (mesh_cmds > 0) ++cull_instance_count;
    }
    cull_total_commands_ = cmd_total;
    // This slot's previous cull finished (the frame fence was waited on), so its
    // count is valid; read it before AddToGraph resets the buffer.
    cull_visible_ = settings_.gpu_culling ? gpu_cull_.last_visible(cull_slot) : cmd_total;
  }
  bool cull_occlusion = settings_.gpu_culling && settings_.gpu_occlusion && has_prev_frame_;
  ResourceHandle cull_hiz =
      cull_occlusion ? gpu_cull_.BuildHiZ(graph_, cull_slot) : kInvalidResource;
  const f32 cull_proj_scale[2] = {proj.m[0], proj.m[5]};
  gpu_cull_.AddToGraph(graph_, view_proj, globals.prev_view_proj, cull_proj_scale, view.camera.eye,
                       cull_instance_count, settings_.gpu_culling, cull_occlusion, cull_hiz,
                       cull_slot);

  // Mesh-shader opaque path: drawn this frame if enabled and supported. The task
  // stage reuses the same hi-z the raster cull built (last frame's) for instance
  // occlusion; ms_occ carries the projection scale + hi-z size (z=0 disables it).
  const bool ms_active = settings_.mesh_shader_lod && mesh_pipeline_->has_mesh_shader();
  const bool ms_occlude = ms_active && cull_occlusion;
  f32 ms_occ[4] = {0, 0, 0, 0};
  if (ms_occlude) {
    ms_occ[0] = proj.m[0];
    ms_occ[1] = proj.m[5];
    ms_occ[2] = static_cast<f32>(gpu_cull_.hiz_width());
    ms_occ[3] = static_cast<f32>(gpu_cull_.hiz_height());
  }
  // Frustum planes for the cpu-side skip of off-screen mesh-shader draws.
  f32 ms_planes[5][4];
  ExtractFrustumPlanes(view_proj, ms_planes);

  // Draws every mesh-shader-eligible mesh; shared by the prepass and scene
  // sub-passes (material binding differs via the bind callbacks).
  auto draw_meshlet_instances = [this, &view, &ms_occ, &ms_planes, &frame](PassContext& ctx) {
    BindingSetHandle bound{};
    for (const DrawItem& item : view.draws) {
      const GpuMesh* mesh = meshes_.find(item.mesh);
      if (!mesh || mesh->all_blend || !mesh->has_meshlets) continue;
      MeshShaderPush push{};
      push.model = item.transform;
      push.prev_model = item.prev_transform;
      push.meshlets_address = mesh->meshlets.address;
      push.meshlet_vertices_address = mesh->meshlet_vertices.address;
      push.meshlet_triangles_address = mesh->meshlet_triangles.address;
      push.vertices_address = mesh->vertices.address;
      push.bounds[0] = mesh->bounds_center[0];
      push.bounds[1] = mesh->bounds_center[1];
      push.bounds[2] = mesh->bounds_center[2];
      push.bounds[3] = mesh->bounds_radius;
      push.occlusion[0] = ms_occ[0];
      push.occlusion[1] = ms_occ[1];
      push.occlusion[2] = ms_occ[2];
      push.occlusion[3] = ms_occ[3];
      // Distance lod pick; the task stage dispatches the chosen lod range.
      Vec3 ms_wc = TransformPoint(
          item.transform, {mesh->bounds_center[0], mesh->bounds_center[1],
                           mesh->bounds_center[2]});
      Vec3 ms_d = view.camera.eye - ms_wc;
      // Cpu frustum skip: a conservative world radius (bounds scaled by the
      // largest transform axis) lets off-screen instances cost no dispatch.
      const f32* m = item.transform.m;
      f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
      f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
      f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
      f32 ms_radius = mesh->bounds_radius * std::max(sx, std::max(sy, sz));
      if (ms_radius > 0.0f && SphereOutsideFrustum(ms_planes, ms_wc, ms_radius)) continue;
      u32 ms_lod =
          SelectLod(*mesh, std::sqrt(ms_d.x * ms_d.x + ms_d.y * ms_d.y + ms_d.z * ms_d.z));
      const base::Vector<GpuSubmesh>& ms_subs =
          ms_lod == 0 ? mesh->submeshes : mesh->lods[ms_lod - 1].submeshes;
      for (const GpuSubmesh& submesh : ms_subs) {
        if (submesh.blend || submesh.meshlet_count == 0) continue;
        BindingSetHandle material = material_system_->set(submesh.material);
        if (!(material == bound)) {
          mesh_pipeline_->BindMeshMaterial(*ctx.cmd, material);
          bound = material;
        }
        push.meshlet_offset = submesh.meshlet_offset;
        push.meshlet_count = submesh.meshlet_count;
        mesh_pipeline_->DrawMeshlets(*ctx.cmd, push);
      }
    }
  };

  graph_.AddPass(
      "prepass",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(normals, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Write(depth_export, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
        if (ms_occlude) builder.Read(cull_hiz, ResourceUsage::kSampledTaskMesh);
      },
      [this, normals, motion, depth_export, depth, cull_commands, &frame, &view, ms_active,
       ms_occlude, cull_hiz, globals_set, update_globals_set, frame_slot,
       draw_meshlet_instances](PassContext& ctx) {
        // First globals-set user this frame: write uniform + tlas + hi-z once.
        update_globals_set(ctx, ms_occlude ? cull_hiz : kInvalidResource, ms_active,
                           /*want_tlas=*/true);

        ColorAttachment colors[3];
        colors[0] = {.view = ctx.graph->image(normals).view};
        colors[1] = {.view = ctx.graph->image(motion).view};
        colors[2] = {.view = ctx.graph->image(depth_export).view};
        DepthAttachment depth_attachment{.view = ctx.graph->image(depth).view,
                                         .clear = 0.0f};  // reversed z clears to far = 0
        ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                 .colors = {colors, 3},
                                 .depth = &depth_attachment,
                                 .shading_rate =
                                     vrs_active_ ? vrs_.rate_view() : TextureView{}});

        // Mesh-shader sub-pass: static opaque meshes, cluster-culled on the gpu.
        if (ms_active) {
          mesh_pipeline_->BindMeshPrepass(*ctx.cmd, globals_set);
          draw_meshlet_instances(ctx);
        }

        // Raster sub-pass: skinned / non-meshlet meshes via gpu-culled indirect
        // draws. Meshes already drawn by the mesh shader are skipped but still
        // advance the cull index so it stays aligned with the cull build order.
        environment_->WriteEnvSet(
            env_prepass_sets_[frame_slot], TextureView{}, nullptr, TextureView{}, GpuBuffer{}, 0,
            TextureView{}, TextureView{}, GpuBuffer{}, 0, TextureView{}, GpuBuffer{}, GpuBuffer{},
            GpuBuffer{}, GpuBuffer{}, TextureView{}, GpuBuffer{}, TextureView{}, TextureView{},
            TextureView{}, TextureView{}, GpuBuffer{}, TextureView{}, TextureView{},
            fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
            fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{});
        mesh_pipeline_->BindPrepass(*ctx.cmd, globals_set, env_prepass_sets_[frame_slot]);
        BindingSetHandle bound_material{};
        bool skinned_bound = false;
        u32 cull_cmd_index = 0;  // matches the cull build order
        for (const DrawItem& item : view.draws) {
          const GpuMesh* mesh = meshes_.find(item.mesh);
          if (!mesh || mesh->all_blend) continue;
          // Stay within the commands the cull build wrote; past that the indirect
          // buffer holds no valid command and reading it renders as garbage.
          if (cull_cmd_index >= cull_total_commands_) break;
          bool ms_handled = ms_active && mesh->has_meshlets;
          bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
          if (!ms_handled) {
            if (draw_skinned != skinned_bound) {
              mesh_pipeline_->SetPrepassSkinned(*ctx.cmd, draw_skinned);
              skinned_bound = draw_skinned;
            }
            MeshPushConstants push{.model = item.transform, .prev_model = item.prev_transform};
            if (mesh->terrain_lod) {
              std::memcpy(push.detail_rect, view.detail_rect, sizeof(push.detail_rect));
            }
            if (draw_skinned && item.skin_offset >= 0) {
              push.bone_address = frame.bone_palette.address;
              push.skin_offset = static_cast<u32>(item.skin_offset);
            }
            mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
          }
          for (const GpuSubmesh& submesh : mesh->submeshes) {
            if (submesh.blend) continue;  // transparency owns its own depth
            if (cull_cmd_index >= cull_total_commands_) break;  // partial-mesh boundary
            if (!ms_handled) {
              BindingSetHandle material = material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexedIndirect(cull_commands,
                                           cull_cmd_index * GpuCull::kCommandStride, 1,
                                           GpuCull::kCommandStride);
            }
            ++cull_cmd_index;
          }
        }
        ctx.cmd->EndRendering();
      });

  // Snapshot this frame's depth for next frame's occlusion test.
  if (settings_.gpu_culling && settings_.gpu_occlusion) {
    gpu_cull_.CopyDepth(graph_, depth_export, cull_slot);
  }

  ResourceHandle ao = kInvalidResource;
  ResourceHandle sun_shadow = kInvalidResource;
  ResourceHandle spec_refl = kInvalidResource;
#if defined(RECREATION_HAS_NRD)
  if (nrd_ao || nrd_shadow) {
    // Shared NRD guides (normal+roughness, viewZ) and per-frame camera state for
    // the REBLUR ao and SIGMA sun-shadow denoisers.
    NrdDenoiser::Inputs nrd_inputs = nrd_.PrepareInputs(graph_, depth_export, normals, 0.1f);
    NrdDenoiser::FrameSettings fs;
    fs.view_to_clip = proj;
    fs.view_to_clip_prev = prev_proj_;
    fs.world_to_view = view_mat;
    fs.world_to_view_prev = prev_view_;
    fs.jitter[0] = jitter_x;
    fs.jitter[1] = jitter_y;
    fs.jitter_prev[0] = prev_jitter_[0];
    fs.jitter_prev[1] = prev_jitter_[1];
    fs.sun_direction = sun;
    fs.frame_index = frame_index_;
    fs.reset = first_frame;
    nrd_.SetFrame(fs);
    if (nrd_ao) {
      // RTAO traces a raw hit distance; REBLUR denoises it.
      ResourceHandle hitdist =
          rtao_.AddToGraph(graph_, *raytracing_, tlas_slot, depth_export, normals,
                           globals.inv_view_proj, frame_index_, 0.1f, NrdDenoiser::kHitDistParams);
      ao = nrd_.DenoiseAo(graph_, nrd_inputs.normal_roughness, nrd_inputs.view_z, motion, hitdist);
    }
    if (nrd_shadow) {
      // Trace a 1-spp cone-jittered sun visibility into SIGMA's penumbra input,
      // then denoise it into a clean screen-space sun shadow the lighting samples
      // (instead of the noisier inline trace the temporal pass had to integrate).
      ResourceHandle penumbra =
          shadow_trace_.AddToGraph(graph_, *raytracing_, tlas_slot, depth_export,
                                   globals.inv_view_proj, sun, 0.1f, settings_.sun_angular_radius,
                                   globals.jitter[0], globals.jitter[1]);
      sun_shadow = nrd_.DenoiseShadow(graph_, nrd_inputs.normal_roughness, nrd_inputs.view_z, motion,
                                      penumbra);
      // Contact shadows: a short screen-space march folded into the denoised
      // shadow, restoring the sub-30cm grounding the 1-spp ray blurs away.
      graph_.AddPass(
          "contact_shadow",
          [&](RenderGraph::PassBuilder& b) {
            b.Write(sun_shadow, ResourceUsage::kStorageWrite);
            b.Read(depth_export, ResourceUsage::kSampledCompute);
          },
          [this, sun_shadow, depth_export, sun, view_proj = globals.view_proj,
           inv_view_proj = globals.inv_view_proj](PassContext& ctx) {
            struct ContactPush {
              Mat4 view_proj;
              Mat4 inv_view_proj;
              f32 sun_dir[3];
              f32 near_plane;
              u32 size[2];
              f32 range;
              f32 thickness;
              u32 steps;
              u32 frame_index;
              f32 pad[2];
            } p{};
            p.view_proj = view_proj;
            p.inv_view_proj = inv_view_proj;
            p.sun_dir[0] = sun.x; p.sun_dir[1] = sun.y; p.sun_dir[2] = sun.z;
            p.near_plane = 0.1f;
            p.size[0] = render_width_; p.size[1] = render_height_;
            p.range = 0.35f;
            p.thickness = 0.4f;
            p.steps = 12;
            p.frame_index = frame_index_;
            ctx.cmd->BindPipeline(contact_shadow_pipeline_);
            ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(sun_shadow)),
                                       Bind::Sampled(1, ctx.graph->image(depth_export))});
            ctx.cmd->Push(p);
            ctx.cmd->Dispatch2D({render_width_, render_height_});
          });
      if (settings_.clouds) {
        // Cloud shadows: the layer's optical depth along the sun ray darkens
        // the same denoised shadow the shading samples.
        graph_.AddPass(
            "cloud_shadow",
            [&](RenderGraph::PassBuilder& b) {
              b.Write(sun_shadow, ResourceUsage::kStorageWrite);
              b.Read(depth_export, ResourceUsage::kSampledCompute);
            },
            [this, sun_shadow, depth_export, sun,
             inv_view_proj = globals.inv_view_proj](PassContext& ctx) {
              struct CloudShadowPush {
                Mat4 inv_view_proj;
                f32 sun_dir[3];
                f32 near_plane;
                u32 size[2];
                f32 time;
                f32 coverage;
                f32 bottom;
                f32 top;
                f32 wind;
                f32 strength;
              } p{};
              p.inv_view_proj = inv_view_proj;
              p.sun_dir[0] = sun.x; p.sun_dir[1] = sun.y; p.sun_dir[2] = sun.z;
              p.near_plane = 0.1f;
              p.size[0] = render_width_; p.size[1] = render_height_;
              p.time = static_cast<f32>(time_seconds_);
              p.coverage = settings_.cloud_coverage;
              p.bottom = 1500.0f;
              p.top = 4200.0f;
              p.wind = 12.0f;
              p.strength = 0.75f;
              ctx.cmd->BindPipeline(cloud_shadow_pipeline_);
              ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(sun_shadow)),
                                         Bind::Sampled(1, ctx.graph->image(depth_export))});
              ctx.cmd->Push(p);
              ctx.cmd->Dispatch2D({render_width_, render_height_});
            });
      }
    }
    if (ddgi_async && spec_refl_active) {
      // First DDGI consumer: the reflection trace samples the probe atlases.
      graph_.AddPass(
          "async_join", [](RenderGraph::PassBuilder& b) { b.JoinAsync(); },
          [](PassContext&) {});
    }
    if (spec_refl_active) {
      // 1-spp VNDF reflection radiance -> REBLUR_SPECULAR; the scene pass
      // samples the result instead of tracing an inline mirror ray.
      EnvironmentSystem::DdgiBinding refl_ddgi;
      if (ddgi_active) refl_ddgi = ddgi_->binding(frame_index_);
      ReflectionTrace::Frame rfl;
      rfl.inv_view_proj = globals.inv_view_proj;
      rfl.camera_pos = view.camera.eye;
      rfl.sun_direction = settings_.sun_direction;
      rfl.sun_intensity = settings_.sun_intensity + settings_.lightning * 9.0f;
      rfl.sun_color = settings_.sun_color;
      rfl.roughness_cutoff = settings_.reflection_roughness_cutoff;
      rfl.frame_index = frame_index_;
      rfl.near_plane = 0.1f;
      rfl.hit_dist_params = NrdDenoiser::kHitDistParams;
      rfl.ddgi = ddgi_active;
      ResourceHandle raw = reflection_trace_.AddToGraph(
          graph_, *raytracing_, tlas_slot, bindless_->set(), depth_export, normals,
          environment_->prefiltered_view(),
          ddgi_active ? refl_ddgi.irradiance : environment_->black_array_view(), ddgi_active,
          ddgi_active ? refl_ddgi.volume : environment_->dummy_volume(),
          ddgi_active ? refl_ddgi.volume_size : 256, environment_->sampler(),
          {render_width_, render_height_}, rfl);
      spec_refl = nrd_.DenoiseSpecular(graph_, nrd_inputs.normal_roughness, nrd_inputs.view_z,
                                       motion, raw);
    }
    prev_proj_ = proj;
    prev_view_ = view_mat;
    prev_jitter_[0] = jitter_x;
    prev_jitter_[1] = jitter_y;
  }
#endif
  if (ss_ao) {
    const f32 proj_scale[2] = {proj.m[0], proj.m[5]};
    ao = ssao_.AddToGraph(graph_, depth_export, normals, globals.inv_view_proj, proj_scale, 0.1f,
                          frame_index_);
  }

  // Hybrid ReSTIR DI: reservoir-resampled point/spot lights with one shadow
  // ray per pixel, shaded off the prepass G-buffer into screen-space targets
  // the forward pass folds back in (env slots 23/24, kFrameFlagRestirDi).
  RestirDi::Outputs restir_out;
  if (restir_active) {
    RestirDi::Frame rf;
    rf.inv_view_proj = globals.inv_view_proj;
    rf.camera_pos = view.camera.eye;
    rf.frame_index = frame_index_;
    rf.light_count = globals.light_count;
    rf.lights = frame.lights;
    rf.tlas_slot = tlas_slot;
    restir_out = restir_di_.AddToGraph(graph_, depth_export, normals, motion, *raytracing_,
                                       {render_width_, render_height_}, rf);
  }

  // Without the reflection trace the scene pass is DDGI's first consumer;
  // join the async queue before the lighting work leading into it.
  if (ddgi_async && spec_refl == kInvalidResource) {
    graph_.AddPass(
        "async_join", [](RenderGraph::PassBuilder& b) { b.JoinAsync(); },
        [](PassContext&) {});
  }

  // Froxel light culling: fixed slots per cluster, run every frame (a zero
  // light count still zeroes the counts the forward loop reads).
  graph_.AddPass(
      "light_cluster",
      [&](RenderGraph::PassBuilder&) {},
      [this, &frame, &view, light_count, decal_count, proj](PassContext& ctx) {
        struct ClusterPush {
          Mat4 view_mat;
          f32 screen[2];
          f32 near_plane;
          f32 slice_scale;
          f32 slice_bias;
          u32 light_count;
          f32 tan_half_fov_y;
          f32 aspect;
          u32 decal_count;
          f32 pad[3];
        } p{};
        p.view_mat = LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
        p.screen[0] = static_cast<f32>(render_width_);
        p.screen[1] = static_cast<f32>(render_height_);
        p.near_plane = 0.1f;
        constexpr f32 kNear = 0.1f, kFar = 500.0f;
        p.slice_scale = static_cast<f32>(kClusterSlices) / std::log2(kFar / kNear);
        p.slice_bias = -std::log2(kNear) * p.slice_scale;
        p.light_count = light_count;
        p.tan_half_fov_y = std::tan(view.camera.fov_y * 0.5f);
        p.aspect = static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
        p.decal_count = decal_count;
        ctx.cmd->BindPipeline(light_cluster_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, frame.lights, 0, frame.lights.size),
                Bind::StorageBuffer(1, cluster_counts_, 0, cluster_counts_.size),
                Bind::StorageBuffer(2, cluster_indices_, 0, cluster_indices_.size),
                Bind::StorageBuffer(3, frame.decals, 0, frame.decals.size),
                Bind::StorageBuffer(4, decal_cluster_indices_, 0,
                                    decal_cluster_indices_.size)});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch((kClusterCount + 63) / 64, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });

  graph_.AddPass(
      "scene",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(scene_color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Write(skin_diffuse, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
        if (ao != kInvalidResource) builder.Read(ao, ResourceUsage::kSampledFragment);
        if (sun_shadow != kInvalidResource) builder.Read(sun_shadow, ResourceUsage::kSampledFragment);
        if (spec_refl != kInvalidResource) builder.Read(spec_refl, ResourceUsage::kSampledFragment);
        if (csm_active) builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
        if (restir_active) {
          builder.Read(restir_out.diffuse, ResourceUsage::kSampledFragment);
          builder.Read(restir_out.spec, ResourceUsage::kSampledFragment);
        }
        if (ms_occlude) builder.Read(cull_hiz, ResourceUsage::kSampledTaskMesh);
      },
      [this, scene_color, motion, skin_diffuse, depth, ao, sun_shadow, spec_refl, use_rt_frag,
       ddgi_active, csm_active, shadow_slot, shadow_atlas, cull_commands, ms_active, globals_set,
       frame_slot, restir_active, restir_out, &frame, &view,
       draw_meshlet_instances](PassContext& ctx) {
        BindingSetHandle env_set = env_scene_sets_[frame_slot];
        TextureView ao_view =
            ao != kInvalidResource ? ctx.graph->image(ao).view : TextureView{};
        TextureView sun_shadow_view =
            sun_shadow != kInvalidResource ? ctx.graph->image(sun_shadow).view : TextureView{};
        TextureView spec_refl_view =
            spec_refl != kInvalidResource ? ctx.graph->image(spec_refl).view : TextureView{};
        EnvironmentSystem::DdgiBinding ddgi_binding;
        if (ddgi_active) ddgi_binding = ddgi_->binding(frame_index_);
        environment_->WriteEnvSet(
            env_set, ao_view, ddgi_active ? &ddgi_binding : nullptr,
            csm_active ? ctx.graph->image(shadow_atlas).view : TextureView{},
            csm_active ? shadow_.cascade_buffer(shadow_slot) : GpuBuffer{},
            shadow_.cascade_buffer_size(), TextureView{}, sun_shadow_view, frame.lights,
            frame.lights.size, spec_refl_view, cluster_counts_, cluster_indices_,
            frame.decals, decal_cluster_indices_, decal_atlas_view_,
            local_shadows_active_ ? local_shadows_.face_buffer(frame_slot) : GpuBuffer{},
            local_shadows_active_ ? local_shadows_.atlas().view : TextureView{},
            decal_normal_atlas_view_,
            restir_active ? ctx.graph->image(restir_out.diffuse).view : TextureView{},
            restir_active ? ctx.graph->image(restir_out.spec).view : TextureView{},
            virtual_texture_.available() ? virtual_texture_.feedback_buffer() : GpuBuffer{},
            virtual_texture_.available() ? virtual_texture_.indirection_view() : TextureView{},
            virtual_texture_.available() ? virtual_texture_.atlas_view() : TextureView{},
            fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
            fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{});

        ColorAttachment colors[3];
        colors[0] = {.view = ctx.graph->image(scene_color).view,
                     .load = LoadOp::kClear,
                     .clear = {0.02f, 0.02f, 0.05f, 1.0f}};
        colors[1] = {.view = ctx.graph->image(motion).view,
                     .load = LoadOp::kLoad};  // the prepass wrote motion
        colors[2] = {.view = ctx.graph->image(skin_diffuse).view,
                     .load = LoadOp::kClear,
                     .clear = {0.0f, 0.0f, 0.0f, 0.0f}};
        DepthAttachment depth_attachment{.view = ctx.graph->image(depth).view,
                                         .load = LoadOp::kLoad};  // prepass depth, tested EQUAL
        ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                 .colors = {colors, 3},
                                 .depth = &depth_attachment});

        BindingSetHandle bindless_set = bindless_ ? bindless_->set() : BindingSetHandle{};

        // Mesh-shader sub-pass: static opaque meshes, finest lod, cluster-culled.
        if (ms_active) {
          mesh_pipeline_->BindMeshScene(*ctx.cmd, globals_set, env_set, bindless_set, use_rt_frag);
          draw_meshlet_instances(ctx);
        }

        mesh_pipeline_->Bind(*ctx.cmd, globals_set, env_set, bindless_set, use_rt_frag,
                             settings_.wireframe);
        BindingSetHandle bound_material{};
        bool skinned_bound = false;
        u32 cull_cmd_index = 0;  // matches the cull build + prepass order
        for (const DrawItem& item : view.draws) {
          const GpuMesh* mesh = meshes_.find(item.mesh);
          if (!mesh || mesh->all_blend) continue;
          if (cull_cmd_index >= cull_total_commands_) break;  // clamp to the built commands
          bool ms_handled = ms_active && mesh->has_meshlets;
          bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
          if (!ms_handled) {
            if (draw_skinned != skinned_bound) {
              mesh_pipeline_->SetSkinned(*ctx.cmd, draw_skinned, use_rt_frag, settings_.wireframe);
              skinned_bound = draw_skinned;
            }
            MeshPushConstants push{.model = item.transform, .prev_model = item.prev_transform};
            if (mesh->terrain_lod) {
              std::memcpy(push.detail_rect, view.detail_rect, sizeof(push.detail_rect));
            }
            if (draw_skinned && item.skin_offset >= 0) {
              push.bone_address = frame.bone_palette.address;
              push.skin_offset = static_cast<u32>(item.skin_offset);
            }
            push.tint_packed = item.tint;  // faction/team colour for skinned actors
            mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
          }
          for (const GpuSubmesh& submesh : mesh->submeshes) {
            if (submesh.blend) continue;
            if (cull_cmd_index >= cull_total_commands_) break;  // partial-mesh boundary
            if (!ms_handled) {
              BindingSetHandle material = material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexedIndirect(cull_commands,
                                           cull_cmd_index * GpuCull::kCommandStride, 1,
                                           GpuCull::kCommandStride);
            }
            ++cull_cmd_index;
          }
        }
        if (skinned_bound) {
          mesh_pipeline_->SetSkinned(*ctx.cmd, false, use_rt_frag, settings_.wireframe);
        }
        if (settings_.sky && !settings_.interior) environment_->DrawSky(*ctx.cmd, globals_set);
        ctx.cmd->EndRendering();
      });

  // Screen-space subsurface scattering: separable per-channel diffusion of the
  // skin buffer the scene pass exported, folded back into the opaque color
  // before anything composites on top.
  if (settings_.sss) {
    struct SssPush {
      u32 size[2];
      f32 inv_size[2];
      f32 dir[2];
      f32 near_plane;
      f32 width;
      f32 proj_scale;
      f32 max_radius;
      u32 composite;
      f32 strength;
    };
    auto fill_sss = [this, proj](SssPush& p) {
      p.size[0] = render_width_;
      p.size[1] = render_height_;
      p.inv_size[0] = 1.0f / static_cast<f32>(render_width_);
      p.inv_size[1] = 1.0f / static_cast<f32>(render_height_);
      p.near_plane = 0.1f;
      p.width = settings_.sss_width;
      // Pixels per meter at view depth 1. The projection bakes the vulkan
      // y-flip into m[5], so it is negative - take the magnitude.
      p.proj_scale = std::abs(proj.m[5]) * 0.5f * static_cast<f32>(render_height_);
      p.max_radius = 24.0f;
      p.strength = std::getenv("REC_SSS_DEBUG") ? -1.0f : 1.0f;  // <0 = mask debug view
    };
    ResourceHandle sss_tmp = graph_.CreateTexture(
        {.name = "sss_tmp", .format = MeshPipeline::kSkinDiffuseFormat,
         .width = render_width_, .height = render_height_});
    graph_.AddPass(
        "sss_blur_h",
        [&](RenderGraph::PassBuilder& b) {
          b.Write(sss_tmp, ResourceUsage::kStorageWrite);
          b.Read(skin_diffuse, ResourceUsage::kSampledCompute);
          b.Read(depth_export, ResourceUsage::kSampledCompute);
        },
        [this, sss_tmp, skin_diffuse, depth_export, fill_sss](PassContext& ctx) {
          SssPush p{};
          fill_sss(p);
          p.dir[0] = 1.0f;
          p.composite = 0;
          ctx.cmd->BindPipeline(sss_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Storage(0, ctx.graph->image(sss_tmp)),
                  Bind::Combined(1, ctx.graph->image(skin_diffuse).view, sss_sampler_),
                  Bind::Combined(2, ctx.graph->image(depth_export).view, sss_sampler_),
                  Bind::Combined(3, ctx.graph->image(skin_diffuse).view, sss_sampler_)});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D({render_width_, render_height_});
        });
    graph_.AddPass(
        "sss_apply",
        [&](RenderGraph::PassBuilder& b) {
          b.Write(scene_color, ResourceUsage::kStorageWrite);
          b.Read(sss_tmp, ResourceUsage::kSampledCompute);
          b.Read(skin_diffuse, ResourceUsage::kSampledCompute);
          b.Read(depth_export, ResourceUsage::kSampledCompute);
        },
        [this, scene_color, sss_tmp, skin_diffuse, depth_export, fill_sss](PassContext& ctx) {
          SssPush p{};
          fill_sss(p);
          p.dir[1] = 1.0f;
          p.composite = 1;
          ctx.cmd->BindPipeline(sss_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Storage(0, ctx.graph->image(scene_color)),
                  Bind::Combined(1, ctx.graph->image(sss_tmp).view, sss_sampler_),
                  Bind::Combined(2, ctx.graph->image(depth_export).view, sss_sampler_),
                  Bind::Combined(3, ctx.graph->image(skin_diffuse).view, sss_sampler_)});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D({render_width_, render_height_});
        });
  }

  // Screen-space gi: add a diffuse bounce over the opaque result before
  // reflections (so reflections pick up the gi-lit color too). Raster tiers only.
  if (ssgi_active && normals != kInvalidResource) {
    const f32 proj_scale[2] = {proj.m[0], proj.m[5]};
    ResourceHandle bounced = ssgi_.AddToGraph(graph_, scene_color, depth_export, normals,
                                              globals.inv_view_proj, proj_scale, 0.1f, frame_index_);
    scene_color = bounced;
    lit = bounced;
  }

  // Screen-space reflections over the opaque result (before transparency, which
  // does not reflect). Replaces scene_color downstream so everything composites
  // onto the reflected image. Only on raster tiers; rt tiers reflect via the tlas.
  if (ssr_active && normals != kInvalidResource) {
    ResourceHandle reflected =
        ssr_.AddToGraph(graph_, scene_color, depth_export, normals, view_proj,
                        globals.inv_view_proj, view.camera.eye, frame_index_);
    scene_color = reflected;
    lit = reflected;
  }

  if (!transparent.empty() && water_) {
    lit = add_water(scene_color, depth, depth_export, motion, sun_shadow, shadow_atlas, csm_active,
                    shadow_slot, tlas_slot, /*globals_written=*/true);
  }

  // Surface weather: wet/darken (rain) or whiten (snow) the lit surfaces before
  // the atmosphere/clouds/rain layer over them. Uses the G-buffer normals + the
  // sky cubemap for the puddle reflection.
  if (settings_.precipitation > 0.0f && !path_trace && normals != kInvalidResource) {
    SurfaceWeather::Frame sf;
    sf.inv_view_proj = globals.inv_view_proj;
    sf.camera_pos = view.camera.eye;
    sf.wetness = settings_.precipitation;
    sf.snow = settings_.precip_snow;
    sf.time = static_cast<f32>(time_seconds_);
    lit = surface_weather_.AddToGraph(graph_, lit, normals, depth_export, environment_->sky_view(),
                                      environment_->sampler(), {render_width_, render_height_}, sf);
  }

  // Aerial perspective: composite the atmosphere between the camera and each
  // surface so distant geometry hazes/blue-shifts like the sky. Cheap; skipped
  // when path tracing (the path tracer scatters its own sky).
  if (settings_.aerial_perspective > 0.0f && !path_trace && !interior) {
    AerialPerspective::Frame af;
    af.inv_view_proj = globals.inv_view_proj;
    af.camera_pos = view.camera.eye;
    af.sun_direction = settings_.sun_direction;
    af.sun_intensity = settings_.sun_intensity;
    af.sun_color = settings_.sun_color;
    af.strength = settings_.aerial_perspective;
    lit = aerial_perspective_.AddToGraph(graph_, lit, depth_export,
                                         environment_->transmittance_view(),
                                         environment_->multiscatter_view(),
                                         {render_width_, render_height_}, af);
  }

  // Volumetric clouds raymarched over the sky, composited against depth so
  // terrain occludes them. Skipped when path tracing.
  if (settings_.clouds && !path_trace && !interior) {
    Clouds::Frame cf;
    cf.inv_view_proj = globals.inv_view_proj;
    cf.camera_pos = view.camera.eye;
    cf.time = static_cast<f32>(time_seconds_);
    cf.sun_direction = settings_.sun_direction;
    // Lightning brightens the cloud (its ambient + sun terms scale with intensity),
    // so the storm clouds flash from within.
    cf.sun_intensity = settings_.sun_intensity + settings_.lightning * 7.0f;
    cf.sun_color = settings_.sun_color;
    cf.coverage = settings_.cloud_coverage;
    lit = clouds_.AddToGraph(graph_, lit, depth_export, {render_width_, render_height_}, cf);
  }

  // Note: screen-space precipitation (rain/snow streaks) is composited after the
  // temporal/upscale resolve below, so TAA's history accumulation does not smear
  // the high-frequency streaks. Surface wetness (above) stays pre-resolve since
  // it shades real surfaces that should be anti-aliased.

  // Unified froxel volumetrics: near-field scattering from the sun and the
  // shadowed clustered lights, composited before the temporal pass so the
  // jittered volume resolves clean.
  bool froxel_on = settings_.froxel_fog && froxel_fog_.available() && !path_trace &&
                   depth_export != kInvalidResource;
  if (froxel_on) {
    FroxelFog::Frame ff;
    ff.inv_view_proj = globals.inv_view_proj;
    ff.prev_view_proj = globals.prev_view_proj;
    ff.camera_pos = view.camera.eye;
    ff.frame_index = frame_index_;
    ff.sun_direction = applied_sun_direction_;
    ff.anisotropy = settings_.fog_anisotropy;
    ff.sun_color = applied_sun_color_ * applied_sun_intensity_;
    ff.ambient = settings_.ambient * 0.05f;
    ff.density = settings_.froxel_density;
    ff.height_falloff = settings_.fog_height_falloff;
    ff.base_height = settings_.fog_base_height;
    std::memcpy(ff.cluster_params, globals.cluster_params, sizeof(ff.cluster_params));
    ff.screen_size[0] = static_cast<f32>(render_width_);
    ff.screen_size[1] = static_cast<f32>(render_height_);
    ff.csm_active = csm_active;
    ff.lights = frame.lights;
    ff.cluster_counts = cluster_counts_;
    ff.cluster_indices = cluster_indices_;
    ff.local_shadow_faces = local_shadows_active_ ? local_shadows_.face_buffer(frame_slot)
                                                  : environment_->dummy_storage();
    ff.local_shadow_atlas = local_shadows_active_ ? local_shadows_.atlas().view
                                                  : environment_->shadow_dummy_view();
    ff.cascade_buffer = csm_active ? shadow_.cascade_buffer(shadow_slot) : GpuBuffer{};
    ff.cascade_size = shadow_.cascade_buffer_size();
    ff.comparison_sampler = environment_->comparison_sampler();
    froxel_fog_.AddToGraph(graph_, lit, depth_export,
                           csm_active ? shadow_atlas : kInvalidResource,
                           {render_width_, render_height_}, ff);
  }

  // Volumetric fog marches the lit scene against depth before the temporal
  // pass, so the marched noise resolves into stable shafts.
  if (fog_active) {
    VolumetricFog::Frame ff;
    ff.inv_view_proj = globals.inv_view_proj;
    ff.camera_pos = view.camera.eye;
    ff.sun_direction = settings_.sun_direction;
    ff.sun_intensity = settings_.sun_intensity;
    ff.sun_color = settings_.sun_color;
    ff.density = settings_.fog_density;
    ff.height_falloff = settings_.fog_height_falloff;
    ff.base_height = settings_.fog_base_height;
    ff.anisotropy = settings_.fog_anisotropy;
    ff.frame_index = frame_index_;
    lit = volumetric_fog_.AddToGraph(graph_, *raytracing_, tlas_slot, lit, depth_export,
                                     {render_width_, render_height_}, ff);
  }

  // Shell fur over the lit scene, depth-tested against the scene depth so the
  // core sphere occludes the far-side shells.
  if (view.fur_ball && !path_trace) {
    Mat4 model{};
    model.m[0] = model.m[5] = model.m[10] = model.m[15] = 1.0f;
    model.m[12] = view.fur_position.x;
    model.m[13] = view.fur_position.y;
    model.m[14] = view.fur_position.z;
    Vec3 sun_col = applied_sun_color_ * applied_sun_intensity_;
    fur_.AddToGraph(graph_, lit, depth, model, view_proj, applied_sun_direction_, sun_col,
                    std::max(settings_.ambient, 0.12f), FurPass::Params{});
  }

  // Order-independent transparency (weighted blended) over the lit scene.
  if (!view.oit.empty() && !path_trace) {
    Vec3 sun_col = applied_sun_color_ * applied_sun_intensity_;
    WboitPass::LightingContext oit_light;
    oit_light.lights = frame.lights;
    oit_light.cluster_counts = cluster_counts_;
    oit_light.cluster_indices = cluster_indices_;
    std::memcpy(oit_light.cluster_params, globals.cluster_params,
                sizeof(oit_light.cluster_params));
    oit_light.froxel_volume = froxel_fog_.integrated().view;
    oit_light.froxel_sampler = froxel_fog_.volume_sampler();
    oit_light.froxel_near = FroxelFog::kNear;
    oit_light.froxel_far = FroxelFog::kFar;
    oit_light.froxel_enabled = froxel_on;
    lit = wboit_.AddToGraph(graph_, lit, depth, view.oit, view_proj, applied_sun_direction_,
                            sun_col, std::max(settings_.ambient, 0.12f), render_width_,
                            render_height_, oit_light);
  }

  // NIF particle emitters: a cpu pool per placed instance of an emitting mesh,
  // stepped here and appended to the frame's billboard sets (lit smoke/mist
  // plus HDR additive fire).
  base::Vector<ParticleInstance> emitter_lit;
  base::Vector<ParticleInstance> emitter_additive;
  if (!mesh_emitters_.empty()) {
    emitter_sim_.BeginFrame(view.frame_delta_seconds, view.camera.eye);
    for (const DrawItem& item : view.draws) {
      if (const auto* emitters = mesh_emitters_.find(item.mesh)) {
        emitter_sim_.AddInstance(item.mesh, *emitters, item.transform);
      }
    }
    emitter_sim_.Simulate(&emitter_lit, &emitter_additive);
  }

  // Lit billboard particles blend over the resolved scene, faded against the
  // prepass depth, before temporal reconstruction. Either a cpu-uploaded set or
  // the gpu-simulated fountain.
  if (!view.particles.empty() || !emitter_lit.empty() || !emitter_additive.empty() ||
      view.gpu_particle_count > 0) {
    Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
    Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
    ParticleSystem::Frame pf;
    pf.view_proj = view_proj;
    pf.prev_view_proj = globals.prev_view_proj;
    pf.cam_right = right;
    pf.cam_up = Cross(right, fwd);
    pf.sun_direction = settings_.sun_direction;
    pf.sun_color = settings_.sun_color;
    pf.sun_intensity = settings_.sun_intensity;
    pf.ambient = std::max(settings_.ambient, 0.15f);
    pf.near_plane = 0.1f;
    pf.soft_fade = 0.6f;
    // Lit translucency inputs: clustered lights + shadows + the fog volume.
    std::memcpy(pf.cluster_params, globals.cluster_params, sizeof(pf.cluster_params));
    pf.froxel_near = FroxelFog::kNear;
    pf.froxel_far = FroxelFog::kFar;
    pf.froxel_enabled = froxel_on;
    pf.lights = frame.lights;
    pf.cluster_counts = cluster_counts_;
    pf.cluster_indices = cluster_indices_;
    pf.local_shadow_faces = local_shadows_active_ ? local_shadows_.face_buffer(frame_slot)
                                                  : environment_->dummy_storage();
    pf.local_shadow_atlas = local_shadows_active_ ? local_shadows_.atlas().view
                                                  : environment_->shadow_dummy_view();
    pf.comparison_sampler = environment_->comparison_sampler();
    pf.froxel_volume = froxel_fog_.integrated().view;
    pf.froxel_sampler = froxel_fog_.volume_sampler();
    if (view.gpu_particle_count > 0) {
      ParticleSystem::Sim sim;
      sim.emitter[0] = view.gpu_particle_emitter.x;
      sim.emitter[1] = view.gpu_particle_emitter.y;
      sim.emitter[2] = view.gpu_particle_emitter.z;
      sim.dt = view.frame_delta_seconds;
      sim.count = view.gpu_particle_count;
      sim.mode = view.gpu_particle_mode;
      sim.radius = view.gpu_particle_radius;
      sim.intensity = view.gpu_particle_intensity;
      sim.time = static_cast<f32>(time_seconds_);
      pf.emissive = view.gpu_particle_mode == 1;
      BindingSetHandle particle_bindless = bindless_ ? bindless_->set() : BindingSetHandle{};
      particles_.SimulateAndDraw(graph_, lit, depth_export, motion, sim, pf, frame_index_ % 2,
                                 particle_bindless);
    } else {
      for (const ParticleInstance& inst : view.particles) emitter_lit.push_back(inst);
      BindingSetHandle particle_bindless = bindless_ ? bindless_->set() : BindingSetHandle{};
      particles_.AddToGraph(graph_, lit, depth_export, motion, emitter_lit, emitter_additive,
                            pf, frame_index_ % 2, particle_bindless);
    }
  }

  // 3D gaussian splats: non-triangle primitives blended over the resolved scene.
  if (!view.gaussians.empty()) {
    GaussianSplat::Frame gf;
    gf.view = view_mat;
    gf.proj_x = proj.m[0];
    gf.proj_y = proj.m[5];
    gf.near_plane = 0.1f;
    gf.screen_x = static_cast<f32>(render_width_);
    gf.screen_y = static_cast<f32>(render_height_);
    gaussians_.AddToGraph(graph_, lit, view.gaussians, gf, frame_index_ % 2);
  }

  // Bounds / acceleration-structure debug view: overlay the cull bounding boxes.
  // Mesh-shader meshlet demo: clusters cull + draw on the gpu, composited into
  // the lit scene with depth. Only active when a meshlet mesh was uploaded.
  if (meshlet_.active()) {
    meshlet_visible_ = meshlet_.last_visible(frame_index_);  // fence-safe, read before reset
    f32 planes[5][4];
    ExtractFrustumPlanes(view_proj, planes);
    meshlet_.AddToGraph(graph_, lit, depth, view_proj, planes, view.camera.eye, frame_index_);
  }
  // Distant foliage imposters: instanced octahedral billboards with depth.
  const char* imposters_env = std::getenv("REC_IMPOSTERS");
  if (imposters_.active() && (!imposters_env || imposters_env[0] != '0')) {
    ImposterPass::Frame imf;
    imf.view_proj = view_proj;
    imf.camera_pos = view.camera.eye;
    imf.sun_direction = applied_sun_direction_;
    imf.sun_intensity = applied_sun_intensity_;
    imf.sun_color = applied_sun_color_;
    imposters_.AddToGraph(graph_, lit, depth, {render_width_, render_height_}, imf);
  }

  // Strand hair: verlet sim + ribbon draw over the lit scene with depth.
  if (hair_.active()) {
    HairStrands::Frame hf;
    hf.view_proj = view_proj;
    hf.camera_pos = view.camera.eye;
    hf.delta_seconds = view.frame_delta_seconds;
    hf.sun_direction = applied_sun_direction_;
    hf.sun_intensity = applied_sun_intensity_;
    hf.sun_color = applied_sun_color_;
    hf.time = static_cast<f32>(time_seconds_);
    hair_.AddToGraph(graph_, lit, depth, {render_width_, render_height_}, hf);
  }

  // Virtual geometry: cluster-DAG LOD cut + cull + draw, all on the gpu.
  if (vgeo_.active()) {
    f32 planes[5][4];
    ExtractFrustumPlanes(view_proj, planes);
    // Screen pixels per world unit at distance 1 (|proj.m5| carries the
    // vulkan y-flip, hence the fabs).
    f32 proj_scale = std::fabs(proj.m[5]) * static_cast<f32>(render_height_) * 0.5f;
    f32 tau = VgeoError.get() > 0.0f ? VgeoError.get() : 1.0f;
    vgeo_.AddToGraph(graph_, lit, depth, view_proj, planes, view.camera.eye, proj_scale, tau,
                     frame_index_);
  }

  if (settings_.debug_view == DebugView::kBounds) {
    gpu_cull_.AddBoundsPass(graph_, lit, view_proj, cull_instance_count, cull_slot);
  }

  // Overdraw debug view: clear lit and additive-replay all geometry so the heat
  // ramp shows how many layers each pixel shaded.
  if (settings_.debug_view == DebugView::kOverdraw) {
    graph_.AddPass(
        "overdraw",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Write(lit, ResourceUsage::kColorAttachment);
        },
        [this, lit, view_proj, &view](PassContext& ctx) {
          overdraw_.Render(
              *ctx.cmd, ctx.graph->image(lit).view, {render_width_, render_height_}, view_proj,
              [this, &view](CommandList& cmd) {
                for (const DrawItem& item : view.draws) {
                  const GpuMesh* mesh = meshes_.find(item.mesh);
                  if (!mesh || !mesh->indices) continue;
                  // view_proj sits at offset 0 (pushed by Render); the model
                  // follows it per draw.
                  cmd.PushConstants(&item.transform, sizeof(Mat4), sizeof(Mat4));
                  cmd.BindVertexBuffer(0, mesh->vertices);
                  cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
                  for (const GpuSubmesh& submesh : mesh->submeshes) {
                    cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
                  }
                }
              });
        });
  }
  }  // end raster path

  // Water has no place in the path tracer (blend geometry never enters the tlas),
  // so composite the raster water pass over the path-traced image. A small opaque
  // depth prepass (direct draws, no gpu cull) gives water correct occlusion and
  // soft shorelines; reflections/shadows trace inline against the path tracer's
  // tlas. The atmosphere/cloud passes that normally precede water are skipped
  // under path tracing, so water is simply the last thing composited.
  if (path_trace && water_pipeline_active && !transparent.empty()) {
    ResourceHandle pt_normals = graph_.CreateTexture(
        {.name = "pt_water_normals", .format = kNormalFormat, .width = render_width_,
         .height = render_height_});
    ResourceHandle pt_motion = graph_.CreateTexture(
        {.name = "pt_water_motion", .format = kMotionFormat, .width = render_width_,
         .height = render_height_});
    ResourceHandle pt_depth = graph_.CreateTexture(
        {.name = "pt_water_depth", .format = kDepthFormat, .width = render_width_,
         .height = render_height_});
    ResourceHandle pt_depth_export = graph_.CreateTexture(
        {.name = "pt_water_depth_export", .format = Format::kR32Float,
         .width = render_width_, .height = render_height_});
    graph_.AddPass(
        "pt_water_prepass",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Write(pt_normals, ResourceUsage::kColorAttachment);
          builder.Write(pt_motion, ResourceUsage::kColorAttachment);
          builder.Write(pt_depth_export, ResourceUsage::kColorAttachment);
          builder.Write(pt_depth, ResourceUsage::kDepthAttachment);
        },
        [this, pt_normals, pt_motion, pt_depth_export, pt_depth, globals_set, update_globals_set,
         frame_slot, &frame, &view](PassContext& ctx) {
          // First globals-set user on the path-traced frame: uniform + tlas (the
          // transparent pass right after wants the tlas for water reflections).
          update_globals_set(ctx, kInvalidResource, false, /*want_tlas=*/true);

          ColorAttachment colors[3];
          colors[0] = {.view = ctx.graph->image(pt_normals).view};
          colors[1] = {.view = ctx.graph->image(pt_motion).view};
          colors[2] = {.view = ctx.graph->image(pt_depth_export).view};
          DepthAttachment depth_attachment{.view = ctx.graph->image(pt_depth).view,
                                           .clear = 0.0f};  // reversed z clears to far = 0
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = {colors, 3},
                                   .depth = &depth_attachment});

          environment_->WriteEnvSet(
            env_prepass_sets_[frame_slot], TextureView{}, nullptr, TextureView{}, GpuBuffer{}, 0,
            TextureView{}, TextureView{}, GpuBuffer{}, 0, TextureView{}, GpuBuffer{}, GpuBuffer{},
            GpuBuffer{}, GpuBuffer{}, TextureView{}, GpuBuffer{}, TextureView{}, TextureView{},
            TextureView{}, TextureView{}, GpuBuffer{}, TextureView{}, TextureView{},
            fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
            fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{});
        mesh_pipeline_->BindPrepass(*ctx.cmd, globals_set, env_prepass_sets_[frame_slot]);
          BindingSetHandle bound_material{};
          bool skinned_bound = false;
          for (const DrawItem& item : view.draws) {
            const GpuMesh* mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend) continue;
            bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
            if (draw_skinned != skinned_bound) {
              mesh_pipeline_->SetPrepassSkinned(*ctx.cmd, draw_skinned);
              skinned_bound = draw_skinned;
            }
            MeshPushConstants push{.model = item.transform, .prev_model = item.prev_transform};
            if (mesh->terrain_lod) {
              std::memcpy(push.detail_rect, view.detail_rect, sizeof(push.detail_rect));
            }
            if (draw_skinned && item.skin_offset >= 0) {
              push.bone_address = frame.bone_palette.address;
              push.skin_offset = static_cast<u32>(item.skin_offset);
            }
            mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
            for (const GpuSubmesh& submesh : mesh->submeshes) {
              if (submesh.blend) continue;  // transparency owns its own depth
              BindingSetHandle material = material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              mesh_pipeline_->DrawSubmesh(*ctx.cmd, submesh);
            }
          }
          ctx.cmd->EndRendering();
        });
    lit = add_water(scene_color, pt_depth, pt_depth_export, pt_motion, kInvalidResource,
                    kInvalidResource, false, 0u, tlas_slot, /*globals_written=*/true);
  }

  // Rebuild next frame's shading rates from the finished render-res frame
  // (post-transparency, pre-resolve: what the scene pass actually shades).
  if (vrs_active_) {
    vrs_.AddToGraph(graph_, lit, motion, {render_width_, render_height_},
                    settings_.vrs_threshold, /*motion_scale=*/2.5f);
  }

  // The path tracer already resolved antialiasing through accumulation; the
  // raster path runs its temporal/upscale resolve here.
  ResourceHandle post_input = lit;
  if (!path_trace) {
  switch (settings_.aa_mode) {
    case AntiAliasingMode::kTaa:
      post_input = taa_.AddToGraph(graph_, lit, motion, frame_index_,
                                   settings_.debug_view == DebugView::kTemporalHistory ? 1u
                                   : settings_.debug_view == DebugView::kMotionVectors  ? 2u
                                                                                        : 0u);
      break;
    case AntiAliasingMode::kUpscaler: {
      ResourceHandle upscaled =
          upscaler_->AddToGraph(graph_, {.color = lit,
                                         .depth = depth_export,
                                         .motion_vectors = motion,
                                         .jitter_x = jitter_x,
                                         .jitter_y = jitter_y,
                                         .sharpness = settings_.sharpness,
                                         .frame_delta_seconds = view.frame_delta_seconds,
                                         .camera_near = 0.1f,
                                         .camera_fov_y = view.camera.fov_y,
                                         .reset_history = first_frame});
      if (upscaled != kInvalidResource) post_input = upscaled;
      break;
    }
    case AntiAliasingMode::kNone:
      break;
  }
  }

  // Dimensions of the aa-resolved image the post stack runs at.
  bool upscaled = !path_trace && settings_.aa_mode == AntiAliasingMode::kUpscaler &&
                  post_input != lit;
  u32 post_width = upscaled ? output_width_ : render_width_;
  u32 post_height = upscaled ? output_height_ : render_height_;

  // Depth of field on the resolved frame, before motion blur streaks over it.
  if (settings_.dof && !path_trace && depth_export != kInvalidResource) {
    DepthOfFieldPass::Frame df;
    df.aperture = settings_.dof_aperture;
    df.focus_distance = settings_.dof_focus;
    post_input = dof_.AddToGraph(graph_, post_input, depth_export,
                                 {post_width, post_height}, df);
  }

  // Motion blur right after the AA resolve (before precipitation streaks and
  // the linear-hdr export). Uses the render-res prepass velocity; uv-space
  // velocities are resolution independent so the upscaled path works too.
  if (settings_.motion_blur && !path_trace && motion != kInvalidResource) {
    MotionBlurPass::Frame mb;
    mb.shutter = settings_.motion_blur_shutter;
    mb.frame_index = frame_index_;
    if (MotionBlurDebugVel.overridden()) {
      mb.debug_velocity[0] = static_cast<f32>(double(MotionBlurDebugVel));
    }
    post_input = motion_blur_.AddToGraph(graph_, post_input, motion,
                                         {post_width, post_height}, mb);
  }

  // Screen-space precipitation streaks, composited at output resolution after the
  // AA resolve so they stay crisp (TAA would otherwise smear them) and tonemap
  // with the scene. Driven by weather; surface wetness was applied pre-resolve.
  if (settings_.precipitation > 0.0f && !path_trace) {
    Precipitation::Frame pf;
    pf.inv_view_proj = globals.inv_view_proj;
    pf.camera_pos = view.camera.eye;
    pf.time = static_cast<f32>(time_seconds_);
    pf.intensity = settings_.precipitation;
    pf.snow = settings_.precip_snow;
    post_input = precipitation_.AddToGraph(graph_, post_input, {post_width, post_height}, pf);
  }

  // Linear-hdr export: copy the resolved scene (pre-tonemap) into a host buffer.
  hdr_pending_ = false;
  if (!hdr_path_.empty() && time_seconds_ >= hdr_at_) {
    u64 need = static_cast<u64>(post_width) * post_height * sizeof(f32) * 4;
    if (hdr_readback_.size != need) {
      device_->DestroyBuffer(hdr_readback_);
      hdr_readback_ = device_->CreateBuffer(need, kBufferUsageStorage, true);
    }
    hdr_width_ = post_width;
    hdr_height_ = post_height;
    hdr_pending_ = hdr_readback_.mapped != nullptr;
    if (hdr_pending_) {
      graph_.AddPass(
          "hdr_capture",
          [&](RenderGraph::PassBuilder& builder) {
            builder.Read(post_input, ResourceUsage::kSampledCompute);
          },
          [this, post_input, post_width, post_height](PassContext& ctx) {
            u32 push[2] = {post_width, post_height};
            ctx.cmd->BindPipeline(hdr_pipeline_);
            ctx.cmd->BindTransient(
                0, {Bind::StorageBuffer(0, hdr_readback_),
                    Bind::Sampled(1, ctx.graph->image(post_input))});
            ctx.cmd->PushConstants(push, sizeof(push));
            ctx.cmd->Dispatch2D({post_width, post_height});
            ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kHostRead);
          });
    }
  }

  exposure_.AddToGraph(graph_, post_input, post_width, post_height, view.frame_delta_seconds);
  ResourceHandle bloom = kInvalidResource;
  if (settings_.bloom) {
    bloom = bloom_.AddToGraph(graph_, post_input, post_width, post_height);
  }

  ResourceHandle backbuffer = graph_.ImportBackbuffer(swapchain_->image(image_index));

  post_->SetGrade(settings_.color_grade);  // rebakes the lut only when it changes
  PostPass::Params post_params{static_cast<u32>(settings_.tonemap), settings_.bloom_intensity,
                               bloom != kInvalidResource ? 1u : 0u,
                               settings_.color_grade != ColorGrade::kNeutral ? 1u : 0u};
  switch (swapchain_->color_space()) {
    case ColorSpace::kHdr10Pq: post_params.output_transfer = 1; break;
    case ColorSpace::kScRgbLinear: post_params.output_transfer = 2; break;
    default: break;
  }
  if (int forced = HdrForceTransfer; forced == 1 || forced == 2) {
    post_params.output_transfer = static_cast<u32>(forced);
  }
  post_params.paper_white = settings_.hdr_paper_white;
  post_params.flare_intensity = settings_.lens_flare;
  post_params.aberration = settings_.chromatic_aberration;
  post_params.vignette = settings_.vignette;
  post_params.grain = settings_.film_grain;
  post_params.grain_seed = static_cast<f32>(frame_index_ % 1024) * 0.6180339f;
  graph_.AddPass(
      "post",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(post_input, ResourceUsage::kSampledFragment);
        if (bloom != kInvalidResource) builder.Read(bloom, ResourceUsage::kSampledFragment);
        builder.Write(backbuffer, ResourceUsage::kColorAttachment);
      },
      [this, post_input, bloom, backbuffer, post_params](PassContext& ctx) {
        TextureView bloom_view = bloom != kInvalidResource ? ctx.graph->image(bloom).view
                                                           : ctx.graph->image(post_input).view;
        post_->Record(ctx, ctx.graph->image(post_input).view, bloom_view,
                      exposure_.exposure_buffer(), exposure_.exposure_buffer_size(),
                      ctx.graph->image(backbuffer).view, ctx.graph->image(backbuffer).extent,
                      post_params);
      });

#if defined(RECREATION_HAS_FSR3)
  // Frame generation: snapshot the pre-UI backbuffer as the interpolation
  // source, so the generated frame carries no smeared UI (it is re-drawn
  // crisp onto the interpolated image in the present path).
  if (fg_active_frame_ && framegen_) {
    graph_.AddPass(
        "fg_hudless",
        [&](RenderGraph::PassBuilder& b) {
          b.Write(backbuffer, ResourceUsage::kColorAttachment);
        },
        [this, backbuffer](PassContext& ctx) {
          const GpuImage& src = ctx.graph->image(backbuffer);
          const GpuImage& dst = framegen_->hudless();
          TextureBarrier pre[] = {
              Transition(src, ResourceState::kColorTarget, ResourceState::kCopySrc),
              Transition(dst, ResourceState::kShaderReadCompute, ResourceState::kCopyDst)};
          ctx.cmd->TextureBarriers(pre);
          ctx.cmd->CopyTexture(src, dst);
          TextureBarrier post[] = {
              Transition(src, ResourceState::kCopySrc, ResourceState::kColorTarget),
              Transition(dst, ResourceState::kCopyDst, ResourceState::kShaderReadCompute)};
          ctx.cmd->TextureBarriers(post);
        });
  }
#endif

  if (view.ui_draw || view.hud_draw) {
    // Backdrop blur: when a frosted widget is present (and the surface lets us
    // sample the backbuffer), capture + Gaussian-blur the post-tonemap frame
    // into ui_frost; the UI backend samples it for frosted panels.
    ResourceHandle ui_frost = kInvalidResource;
    if (view.hud_draw && view.needs_blur && ui_blur_ && swapchain_->can_sample()) {
      ui_frost = ui_blur_->AddToGraph(graph_, backbuffer, output_width_, output_height_);
    }

    graph_.AddPass(
        "ui",
        [&](RenderGraph::PassBuilder& builder) {
          if (ui_frost != kInvalidResource)
            builder.Read(ui_frost, ResourceUsage::kSampledFragment);
          builder.Write(backbuffer, ResourceUsage::kColorAttachment);
        },
        [this, backbuffer, ui_frost, &view](PassContext& ctx) {
          ColorAttachment color{.view = ctx.graph->image(backbuffer).view, .load = LoadOp::kLoad};
          // Hand the blurred backdrop to the UI before it records (the closure
          // reads view.blur_source); null when blur is not in play this frame.
          view.blur_source =
              ui_frost != kInvalidResource ? ctx.graph->image(ui_frost).view : TextureView{};
          view.blur_sampler = ui_frost != kInvalidResource ? ui_blur_->sampler() : SamplerHandle{};
          ctx.cmd->BeginRendering({.extent = ctx.graph->image(backbuffer).extent,
                                   .colors = {&color, 1}});
          if (view.hud_draw) view.hud_draw(*ctx.cmd);
          if (view.ui_draw) view.ui_draw(*ctx.cmd);
          ctx.cmd->EndRendering();
        });
  }
}

bool Renderer::CreateFrameResources() {
  for (FrameResources& frame : frames_) {
    frame.globals = device_->CreateBuffer(sizeof(FrameGlobals), kBufferUsageUniform, true);
    if (!frame.globals.mapped) return false;

    // Bone palette: host visible, read in the skinned vertex shader through its
    // device address (no descriptor binding). Column-major 4x4 per bone.
    frame.bone_palette = device_->CreateBuffer(
        static_cast<u64>(kMaxFrameBones) * sizeof(Mat4),
        kBufferUsageStorage | kBufferUsageDeviceAddress, true);
    if (!frame.bone_palette.mapped) return false;

    frame.lights = device_->CreateBuffer(static_cast<u64>(kMaxFrameLights) * sizeof(PointLight),
                                         kBufferUsageStorage, true);
    if (!frame.lights.mapped) return false;
    frame.decals = device_->CreateBuffer(static_cast<u64>(kMaxFrameDecals) * sizeof(Decal),
                                         kBufferUsageStorage, true);
    if (!frame.decals.mapped) return false;
  }
  return true;
}

void Renderer::DestroyFrameResources() {
  for (FrameResources& frame : frames_) {
    if (frame.globals) device_->DestroyBuffer(frame.globals);
    if (frame.bone_palette) device_->DestroyBuffer(frame.bone_palette);
    if (frame.lights) device_->DestroyBuffer(frame.lights);
    if (frame.decals) device_->DestroyBuffer(frame.decals);
    frame = {};
  }
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device_->DestroyBindingSet(globals_sets_[i]);
    device_->DestroyBindingSet(env_scene_sets_[i]);
    device_->DestroyBindingSet(env_prepass_sets_[i]);
    device_->DestroyBindingSet(env_transparent_sets_[i]);
    globals_sets_[i] = {};
    env_scene_sets_[i] = {};
    env_transparent_sets_[i] = {};
  }
}

void Renderer::RecreateSwapchain() {
  u32 width = window_->width();
  u32 height = window_->height();
  if (width == 0 || height == 0) return;  // minimized
  device_->WaitIdle();
  swapchain_.reset();
  swapchain_ = device_->CreateSwapchain(width, height, settings_.vsync,
                                        settings_.hdr_output);
  if (!swapchain_) return;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  // The upscaler is sized for the output, rebuild it alongside.
  if (upscaler_) {
    upscaler_.reset();
    if (!CreateUpscalerForSettings()) {
      settings_.upscaler = UpscalerKind::kNone;
      settings_.aa_mode = AntiAliasingMode::kTaa;
      applied_upscaler_ = UpscalerKind::kNone;
    }
  }
  // The frame generator is sized for the swapchain; lazily recreated.
  framegen_.reset();
  framegen_attempted_ = false;
  framegen_was_active_ = false;
  UpdateRenderResolution();
  transient_pool_->Clear();
  // Resize every render-resolution pass through the shared helper rather than a
  // partial hand-rolled copy: this list had drifted and omitted the NRD
  // denoiser (and vrs/restir/rr), so the SIGMA sun-shadow history stayed at the
  // old resolution - shadows kept the pre-resize size and ghosted at the wrong
  // framebuffer position until the history flushed.
  ResizeSizedPasses();
  taa_.Reset();
  has_prev_frame_ = false;
}

void Renderer::DestroySurface() {
  if (!device_ || device_->is_stub()) return;
  device_->WaitIdle();
  swapchain_.reset();
  device_->DestroySurface();
}

void Renderer::RecreateSurface() {
  if (!device_ || device_->is_stub() || !window_) return;
  if (!device_->RecreateSurface(*window_)) return;
  RecreateSwapchain();  // rebuilds the swapchain and sized targets
}

void Renderer::WaitIdle() {
  if (device_ && !device_->is_stub()) device_->WaitIdle();
}

void Renderer::Shutdown() {
  if (device_ && !device_->is_stub()) {
    device_->WaitIdle();
    DestroyFrameResources();
    for (auto kv : meshes_) {
      device_->DestroyBuffer(kv.value.vertices);
      device_->DestroyBuffer(kv.value.indices);
      if (kv.value.skinning) device_->DestroyBuffer(kv.value.skinning);
      if (kv.value.meshlets) device_->DestroyBuffer(kv.value.meshlets);
      if (kv.value.meshlet_vertices) device_->DestroyBuffer(kv.value.meshlet_vertices);
      if (kv.value.meshlet_triangles) device_->DestroyBuffer(kv.value.meshlet_triangles);
    }
    meshes_.clear();
    taa_.Destroy(*device_);
    ssao_.Destroy(*device_);
    ssr_.Destroy(*device_);
    ssgi_.Destroy(*device_);
    device_->DestroyPipeline(hdr_pipeline_);
    hdr_pipeline_ = {};
    device_->DestroyBuffer(hdr_readback_);
    shadow_.Destroy(*device_);
    local_shadows_.Destroy(*device_);
    froxel_fog_.Destroy(*device_);
    particles_.Destroy(*device_);
    gaussians_.Destroy(*device_);
    fur_.Destroy(*device_);
    wboit_.Destroy(*device_);
    overdraw_.Destroy(*device_);
    gpu_cull_.Destroy(*device_);
    meshlet_.Destroy(*device_);
    if (ms_dummy_hiz_) device_->DestroyImage(ms_dummy_hiz_);
    if (rt_available_) rtao_.Destroy(*device_);
    if (rt_available_) reflection_trace_.Destroy(*device_);
    motion_blur_.Destroy(*device_);
    dof_.Destroy(*device_);
    if (light_cluster_pipeline_) device_->DestroyPipeline(light_cluster_pipeline_);
    if (contact_shadow_pipeline_) device_->DestroyPipeline(contact_shadow_pipeline_);
    if (cloud_shadow_pipeline_) device_->DestroyPipeline(cloud_shadow_pipeline_);
    if (sss_pipeline_) device_->DestroyPipeline(sss_pipeline_);
    if (cluster_counts_) device_->DestroyBuffer(cluster_counts_);
    if (cluster_indices_) device_->DestroyBuffer(cluster_indices_);
    if (decal_cluster_indices_) device_->DestroyBuffer(decal_cluster_indices_);
#if defined(RECREATION_HAS_NRD)
    if (rt_available_) nrd_.Destroy(*device_);
    if (rt_available_) shadow_trace_.Destroy(*device_);
#endif
#if defined(RECREATION_HAS_DLSS)
    rr_.Destroy(*device_);
#endif
    bloom_.Destroy(*device_);
    exposure_.Destroy(*device_);
    vrs_.Destroy(*device_);
    restir_di_.Destroy(*device_);
    virtual_texture_.Destroy(*device_);
    vgeo_.Destroy(*device_);
    hair_.Destroy(*device_);
    ocean_.Destroy(*device_);
    imposters_.Destroy(*device_);
    profiler_.Shutdown();
    path_tracer_.Destroy(*device_);
    recon_path_tracer_.Destroy(*device_);
    volumetric_fog_.Destroy(*device_);
    aerial_perspective_.Destroy(*device_);
    clouds_.Destroy(*device_);
    precipitation_.Destroy(*device_);
    surface_weather_.Destroy(*device_);
    water_.reset();
    ddgi_.reset();
    environment_.reset();
    material_system_.reset();
    bindless_.reset();
    transient_pool_.reset();
  }
  graph_.Reset();
  post_.reset();
  ui_blur_.reset();  // holds a Device& + backend handles; destroy before device_
  mesh_pipeline_.reset();
  swapchain_.reset();
  framegen_.reset();  // ffx contexts destroy through the device
  upscaler_.reset();
  raytracing_.reset();
  device_.reset();
}

const DeviceCaps* Renderer::caps() const { return device_ ? &device_->caps() : nullptr; }

Format Renderer::swapchain_format() const {
  return swapchain_ ? swapchain_->format() : Format::kUnknown;
}

u32 Renderer::swapchain_image_count() const {
  return swapchain_ ? swapchain_->image_count() : 0;
}

}  // namespace rec::render

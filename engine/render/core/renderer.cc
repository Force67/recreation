#include "render/core/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <base/option.h>

#include <stb_image_write.h>

#include "asset/primitives.h"
#include "core/log.h"
#include "render/util/exr_write.h"
#include "render/util/shader_util.h"
#include "shaders/hdr_capture_cs_hlsl.h"

namespace rec::render {
namespace {

// Renderer config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup (see Engine::Initialize). DebugView and
// ColorGrade options take an Opt suffix to avoid shadowing the enums of the same
// name used in the casts below.
base::Option<const char*> Screenshot{"screenshot", nullptr, "REC_SCREENSHOT"};
base::Option<const char*> Hdr{"hdr", nullptr, "REC_HDR"};
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
base::Option<bool> Fog{"fog", false, "REC_FOG"};
base::Option<float> Aerial{"aerial", 1.0f, "REC_AERIAL"};
base::Option<bool> CloudsOpt{"clouds", false, "REC_CLOUDS"};
base::Option<float> CloudCoverage{"cloud.coverage", 0.46f, "REC_CLOUD_COVERAGE"};
base::Option<float> Precip{"precip", 0.0f, "REC_PRECIP"};
base::Option<bool> Snow{"snow", false, "REC_SNOW"};
base::Option<bool> Aurora{"aurora", false, "REC_AURORA"};

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

  window_ = &window;
  device_ = Device::Create({.enable_validation = desc.enable_validation,
                            .request_raytracing = desc.enable_raytracing},
                           window);
  if (device_->is_stub()) {
    REC_WARN("renderer running in stub mode");
    return true;
  }

  swapchain_ = Swapchain::Create(*device_, output_width_, output_height_, settings_.vsync);
  if (!swapchain_ || !CreateFrameResources() || !CreateRenderFinishedSemaphores()) return false;
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
                                        bindless_ ? bindless_->set_layout() : VK_NULL_HANDLE);
  post_ = PostPass::Create(*device_, swapchain_->format());
  if (!mesh_pipeline_ || !post_ || !taa_.Initialize(*device_)) return false;
  ui_blur_ = UiBlurPass::Create(*device_);  // optional: frosted-glass UI blur
  if (rt_available_ && !rtao_.Initialize(*device_)) return false;
  if (!ssao_.Initialize(*device_)) return false;  // raster ao fallback, no rt needed
  if (!ssr_.Initialize(*device_)) return false;   // raster reflection fallback
  if (!ssgi_.Initialize(*device_)) return false;  // raster diffuse-gi fallback

  // Linear-hdr export: a compute copy from the resolved scene into a host buffer.
  {
    VkDescriptorSetLayoutBinding hb[2]{};
    hb[0].binding = 0;
    hb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    hb[0].descriptorCount = 1;
    hb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    hb[1].binding = 1;
    hb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    hb[1].descriptorCount = 1;
    hb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo si{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 2;
    si.pBindings = hb;
    if (vkCreateDescriptorSetLayout(device_->device(), &si, nullptr, &hdr_set_layout_) != VK_SUCCESS)
      return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(u32)};
    VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &hdr_set_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(device_->device(), &li, nullptr, &hdr_layout_) != VK_SUCCESS)
      return false;
    VkShaderModule m =
        CreateShaderModule(device_->device(), k_hdr_capture_cs_hlsl, sizeof(k_hdr_capture_cs_hlsl));
    if (m == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo ci{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = hdr_layout_;
    VkResult r =
        vkCreateComputePipelines(device_->device(), VK_NULL_HANDLE, 1, &ci, nullptr, &hdr_pipeline_);
    vkDestroyShaderModule(device_->device(), m, nullptr);
    if (r != VK_SUCCESS) return false;
  }
  if (!shadow_.Initialize(*device_, material_system_->set_layout())) return false;  // raster sun shadows
  if (!particles_.Initialize(*device_, kSceneColorFormat)) return false;
  if (!gaussians_.Initialize(*device_, kSceneColorFormat)) return false;
  if (!fur_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!wboit_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (!overdraw_.Initialize(*device_, kSceneColorFormat)) return false;
  if (!gpu_cull_.Initialize(*device_, kSceneColorFormat)) return false;
  if (!meshlet_.Initialize(*device_, kSceneColorFormat, kDepthFormat)) return false;
  if (device_->caps().mesh_shaders) {
    // 1x1 fallback hi-z so the mesh-shader cull descriptor is always valid; bound
    // (with occlusion disabled) on frames where no real hi-z was built.
    ms_dummy_hiz_ = device_->CreateImage2D(VK_FORMAT_R32_SFLOAT, {1, 1},
                                           VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    device_->ImmediateSubmit([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      b.dstStageMask = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.image = ms_dummy_hiz_.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
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
  }
  if (rt_available_) volumetric_fog_.Initialize(*device_);
  aerial_perspective_.Initialize(*device_);  // atmospheric distance haze (no ray tracing)
  clouds_.Initialize(*device_);              // volumetric clouds (no ray tracing)
  precipitation_.Initialize(*device_);       // screen-space rain/snow
  surface_weather_.Initialize(*device_);     // rain wetness / snow accumulation

  UpdateRenderResolution();
  taa_.Resize(*device_, {render_width_, render_height_});
  ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
  path_tracer_.Resize(*device_, {render_width_, render_height_});
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
  // REC_MESH_SHADER_LOD=1 opts into the optional VK_EXT_mesh_shader opaque path.
  if (MeshShaderLod.overridden()) settings_.mesh_shader_lod = MeshShaderLod;
  // Hardware gate: the path needs VK_EXT_mesh_shader and its pipelines to have
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
  if (PathtraceSpp.overridden()) settings_.path_trace_spp = static_cast<u32>(std::max(1, int(PathtraceSpp)));
  if (PathtraceAccum.overridden()) settings_.path_trace_accum = static_cast<u32>(std::max(1, int(PathtraceAccum)));
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

void Renderer::WriteScreenshot(u32 image_index) {
  device_->WaitIdle();
  VkExtent2D extent = swapchain_->extent();
  u64 size = static_cast<u64>(extent.width) * extent.height * 4;
  GpuBuffer staging = device_->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  if (!staging.mapped) return;

  device_->ImmediateSubmit([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = swapchain_->image(image_index);
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, swapchain_->image(image_index),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &dep);
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
    render_width_ = output_width_;
    render_height_ = output_height_;
  }
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
                          settings_.upscaler_quality != applied_quality_;
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
    UpdateRenderResolution();
    transient_pool_->Clear();
    taa_.Resize(*device_, {render_width_, render_height_});
    ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
    path_tracer_.Resize(*device_, {render_width_, render_height_});
    if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
#if defined(RECREATION_HAS_NRD)
    if (rt_available_ && nrd_.available()) nrd_.Resize(*device_, {render_width_, render_height_});
    if (rt_available_) shadow_trace_.Resize(*device_, {render_width_, render_height_});
#endif
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
      taa_.Resize(*device_, {render_width_, render_height_});
      ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
      path_tracer_.Resize(*device_, {render_width_, render_height_});
      if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
#if defined(RECREATION_HAS_NRD)
      if (rt_available_ && nrd_.available()) nrd_.Resize(*device_, {render_width_, render_height_});
      if (rt_available_) shadow_trace_.Resize(*device_, {render_width_, render_height_});
#endif
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

bool Renderer::UploadMesh(const asset::Mesh& mesh, u64 id_salt) {
  if (!device_ || device_->is_stub()) return false;
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) return false;
  const u64 mesh_key = mesh.id.hash ^ id_salt;

  VkBufferUsageFlags rt_usage =
      raytracing_ ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                  : 0;
  // The mesh-shader path reads vertices/meshlets by device address.
  const bool build_meshlets = device_->caps().mesh_shaders && !mesh.skinned;
  VkBufferUsageFlags ms_usage = build_meshlets ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0;
  auto buffer_address = [&](const GpuBuffer& b) -> u64 {
    VkBufferDeviceAddressInfo ai{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b.buffer;
    return vkGetBufferDeviceAddress(device_->device(), &ai);
  };

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
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rt_usage | ms_usage);
  gpu.indices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(all_indices.data()), all_indices.size() * sizeof(u32)),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rt_usage);
  gpu.index_count = static_cast<u32>(lod.indices.size());    // lod 0 (rt/shadow/overdraw)
  gpu.vertex_count = static_cast<u32>(lod.vertices.size());
  // Skinned meshes carry a parallel bone index/weight stream, bound as a second
  // vertex buffer by the skinned pipeline. Skinned meshes are not lod'd.
  if (mesh.skinned && lod.skinning.size() == lod.vertices.size()) {
    gpu.skinning = device_->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8*>(lod.skinning.data()),
                 lod.skinning.size() * sizeof(asset::SkinnedVertexExtra)),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpu.skinned = gpu.skinning.buffer != VK_NULL_HANDLE;
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
      out.push_back({index_base + submesh.index_offset, submesh.index_count, material,
                     blend, water, mask});
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
      gpu.meshlets_address = buffer_address(gpu.meshlets);
      gpu.meshlet_vertices_address = buffer_address(gpu.meshlet_vertices);
      gpu.meshlet_triangles_address = buffer_address(gpu.meshlet_triangles);
      gpu.vertices_address = buffer_address(gpu.vertices);
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
    gpu.bindless_index = bindless_->RegisterMesh(gpu.vertices.buffer, gpu.indices.buffer,
                                                 geometries.data(),
                                                 static_cast<u32>(geometries.size()));
    if (gpu.bindless_index == BindlessRegistry::kInvalidIndex) gpu.bindless_index = 0;
  }
  meshes_[mesh_key] = gpu;
  // Pure transparency never enters the tlas: water occluding rtao and
  // shadow rays would black out everything under it.
  if (raytracing_ && !gpu.all_blend && include_rt) raytracing_->BuildBlas(mesh_key, gpu);
  return true;
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

  ApplySettings();

  FrameResources& frame = frames_[frame_index_ % kFramesInFlight];
  vkWaitForFences(device_->device(), 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

  u32 image_index = 0;
  VkResult acquired = swapchain_->Acquire(frame.image_available, &image_index);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    RecreateSwapchain();
    return;
  }
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return;

  vkResetCommandPool(device_->device(), frame.pool, 0);
  vkResetDescriptorPool(device_->device(), frame.descriptor_pool, 0);

  transient_pool_->BeginFrame();
  graph_.Reset();
  BuildFrameGraph(frame, image_index, view);
  if (!graph_.Compile(*device_, *transient_pool_)) return;

  // Only reset once the frame is guaranteed to submit, so an early return
  // above cannot deadlock the next wait.
  vkResetFences(device_->device(), 1, &frame.in_flight);

  VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.cmd, &begin);

  profiler_.BeginFrame(frame.cmd, frame_index_ % kFramesInFlight);
  graph_.SetPassHooks(
      [this](VkCommandBuffer cmd, const char* name) { profiler_.BeginPass(cmd, name); },
      [this](VkCommandBuffer cmd) { profiler_.EndPass(cmd); });

  PassContext ctx;
  ctx.cmd = frame.cmd;
  ctx.device = device_.get();
  ctx.allocate_set = [this, &frame](VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = frame.descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device_->device(), &info, &set);
    return set;
  };
  graph_.Execute(ctx);

  vkEndCommandBuffer(frame.cmd);

  VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = frame.image_available;
  wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = render_finished_[image_index];
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cmd_info.commandBuffer = frame.cmd;

  VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &wait;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd_info;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  vkQueueSubmit2(device_->graphics_queue(), 1, &submit, frame.in_flight);

  if (!screenshot_path_.empty() && time_seconds_ >= screenshot_at_) {
    WriteScreenshot(image_index);
  }
  if (hdr_pending_) {
    WriteHdr();
    hdr_pending_ = false;
  }

  VkResult presented = swapchain_->Present(render_finished_[image_index], image_index);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
    RecreateSwapchain();
  } else if (presented == VK_SUBOPTIMAL_KHR) {
    // Android reports SUBOPTIMAL every frame because preTransform (identity)
    // differs from the panel's rotation, which is stable and handled by the
    // display engine; recreating on that alone would rebuild every frame. Only
    // recreate when the surface extent actually changed (a real resize).
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical_device(), device_->surface(), &caps);
    if (caps.currentExtent.width != 0xffffffffu &&
        (caps.currentExtent.width != swapchain_->extent().width ||
         caps.currentExtent.height != swapchain_->extent().height)) {
      RecreateSwapchain();
    }
  }
  ++frame_index_;
}

void Renderer::BuildFrameGraph(FrameResources& frame, u32 image_index, const FrameView& view) {
  bool rt_shadows = rt_available_ && settings_.rt_shadows;
  bool rtao_active = rt_available_ && settings_.rtao;
  bool ddgi_active = ddgi_ && settings_.ddgi && settings_.ibl;
  bool reflections_active = rt_available_ && settings_.rt_reflections && bindless_ != nullptr;
  // The ray-query fragment variant serves both shadows and reflections.
  bool use_rt_frag = rt_shadows || reflections_active;
  bool path_trace = rt_available_ && bindless_ != nullptr && settings_.path_trace;
  bool fog_active = rt_available_ && settings_.fog && !path_trace;
  // Ambient occlusion technique: ray-traced + NRD-denoised when available, else
  // the screen-space fallback so non-rt tiers (and forced low presets) keep ao.
  bool nrd_ao = false;
  bool nrd_shadow = false;
#if defined(RECREATION_HAS_NRD)
  nrd_ao = rtao_active && nrd_.available();
  nrd_shadow = rt_shadows && nrd_.available();
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
  Vec3 sun = Normalize(settings_.sun_direction);
  globals.sun_direction[0] = sun.x;
  globals.sun_direction[1] = sun.y;
  globals.sun_direction[2] = sun.z;
  // Lightning flashes the PER-FRAME direct light only (not settings_, so the
  // sun-change check never rebuilds the IBL cubemap): a brief bright blue-white
  // boost to the directional intensity, colour and ambient fill.
  const f32 flash = settings_.lightning;
  globals.sun_direction[3] = settings_.sun_intensity + flash * 9.0f;
  globals.sun_color[0] = settings_.sun_color.x + flash * (0.90f - settings_.sun_color.x);
  globals.sun_color[1] = settings_.sun_color.y + flash * (0.95f - settings_.sun_color.y);
  globals.sun_color[2] = settings_.sun_color.z + flash * (1.10f - settings_.sun_color.z);
  globals.sun_color[3] = settings_.ambient + flash * 0.5f;
  globals.camera_position[0] = view.camera.eye.x;
  globals.camera_position[1] = view.camera.eye.y;
  globals.camera_position[2] = view.camera.eye.z;
  globals.camera_position[3] = settings_.ibl_intensity;
  globals.misc[0] = static_cast<f32>(render_width_);
  globals.misc[1] = static_cast<f32>(render_height_);
  globals.misc[2] = settings_.sun_angular_radius;
  globals.misc[3] = static_cast<f32>(frame_index_ % 4096);
  if (settings_.ibl) globals.flags |= kFrameFlagIbl;
  if (nrd_ao || ss_ao) globals.flags |= kFrameFlagAoValid;
  if (csm_active) globals.flags |= kFrameFlagShadowMap;
  if (ddgi_active) globals.flags |= kFrameFlagDdgi;
  if (water_pipeline_active && settings_.water_reflections) globals.flags |= kFrameFlagWaterRt;
  if (rt_shadows) globals.flags |= kFrameFlagRtShadows;
  if (settings_.aurora) globals.flags |= kFrameFlagAurora;
  if (nrd_shadow) globals.flags |= kFrameFlagSigmaShadow;
  if (reflections_active) globals.flags |= kFrameFlagReflections;
  globals.time = static_cast<f32>(time_seconds_);
  globals.debug_view = static_cast<u32>(settings_.debug_view);
  globals.reflection_cutoff = settings_.reflection_roughness_cutoff;
  globals.ao_ray_count = nrd_ao ? settings_.ao_rays : 0u;  // rt ao rays, for the ray-count view

  // Dynamic point lights: copy into the host-visible frame buffer (capped).
  u32 light_count = std::min<u32>(static_cast<u32>(view.lights.size()), kMaxFrameLights);
  if (light_count > 0) {
    std::memcpy(frame.lights.mapped, view.lights.data(), light_count * sizeof(PointLight));
  }
  globals.light_count = light_count;
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
                                         .format = VK_FORMAT_D32_SFLOAT,
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
        {.name = "depth_export", .format = VK_FORMAT_R32_SFLOAT, .width = render_width_,
         .height = render_height_});
  }

  if (environment_dirty_ && (settings_.ibl || settings_.sky)) {
    environment_dirty_ = false;
    Vec3 env_sun = applied_sun_direction_;
    f32 env_intensity = applied_sun_intensity_;
    Vec3 env_color = applied_sun_color_;
    graph_.AddPass(
        "env_update", [](RenderGraph::PassBuilder&) {},
        [this, env_sun, env_intensity, env_color](PassContext& ctx) {
          environment_->RecordUpdate(ctx.cmd, env_sun, env_intensity, env_color);
        });
  }

  u32 tlas_slot = frame_index_ % RayTracingContext::kSlots;
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
    // stalls the device or frees buffers mid command buffer (which races the
    // frame ring and corrupts the image). Spikes here when two worlds stream in.
    raytracing_->ReserveTlas(tlas_slot, static_cast<u32>(instances.size()));
    graph_.AddPass(
        "tlas_build", [](RenderGraph::PassBuilder&) {},
        [this, tlas_slot, instances = std::move(instances)](PassContext& ctx) {
          raytracing_->BuildTlas(ctx.cmd, tlas_slot, instances);
        });
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
#if defined(RECREATION_HAS_NRD)
    if (nrd_.available() && !settings_.path_trace_reference) {
      // Playable: spp lighting samples, then NRD's REBLUR_DIFFUSE reprojects
      // history across camera motion (no full reset), so the view stays clean
      // while moving. More spp = lower input variance = less shimmer.
      denoised_path = true;
      pt.spp = settings_.path_trace_spp;
      // Ray-cone spread for texture lod: vertical fov radians per pixel.
      pt.pixel_spread = 2.0f * std::tan(view.camera.fov_y * 0.5f) / static_cast<f32>(render_height_);
      PathTracer::GbufferTargets t;
      auto guide = [&](const char* name, VkFormat format) {
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
      // through its own temporal accumulation + antilag instead.
      fs.reset = first_frame || !pt_was_active_;
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
    if (!denoised_path) {
      // Reference: brute-force accumulation, hard reset on any motion = ground truth.
      pt.reset = !pt_was_active_ || moved || lit_changed;
      path_tracer_.AddToGraph(graph_, *raytracing_, tlas_slot, bindless_->set(),
                              environment_->sky_view(), environment_->sampler(), scene_color, pt);
    }
    pt_prev_view_proj_ = view_proj;
    pt_prev_sig_ = sig;
    pt_was_active_ = true;
  } else {
    pt_was_active_ = false;

  u32 shadow_slot = frame_index_ % 2;
  if (csm_active) {
    Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
    Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
    Vec3 up = Cross(right, fwd);
    f32 aspect = static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
    shadow_.Update(view.camera.eye, fwd, right, up, view.camera.fov_y, aspect,
                   settings_.sun_direction, shadow_slot);
    graph_.AddPass(
        "shadow_cascades",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Write(shadow_atlas, ResourceUsage::kDepthAttachment);
        },
        [this, shadow_atlas, &frame, &view](PassContext& ctx) {
          VkImageView atlas = ctx.graph->image(shadow_atlas).view;
          shadow_.Render(
              ctx.cmd, atlas, [this, &frame, &view](VkCommandBuffer cmd, VkPipelineLayout layout) {
                VkDescriptorSet bound_material = VK_NULL_HANDLE;
                VkPipeline bound_pipeline = VK_NULL_HANDLE;
                for (const DrawItem& item : view.draws) {
                  const GpuMesh* mesh = meshes_.find(item.mesh);
                  // no_rt skips grass-like fill geometry, but skinned actors are
                  // no_rt only to stay out of the tlas; they still cast shadows.
                  if (!mesh || mesh->all_blend || (mesh->no_rt && !mesh->skinned)) continue;
                  // Skinned casters run the bone-blended vertex stage so the
                  // shadow tracks the animated pose, not the bind pose.
                  bool draw_skinned = mesh->skinned && item.skin_offset >= 0 &&
                                      shadow_.skinned_pipeline() != VK_NULL_HANDLE;
                  VkPipeline pipeline =
                      draw_skinned ? shadow_.skinned_pipeline() : shadow_.pipeline();
                  if (pipeline != bound_pipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    bound_pipeline = pipeline;
                  }
                  vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(Mat4),
                                     sizeof(Mat4), &item.transform);
                  VkDeviceSize offset = 0;
                  vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices.buffer, &offset);
                  if (draw_skinned) {
                    vkCmdBindVertexBuffers(cmd, 1, 1, &mesh->skinning.buffer, &offset);
                    struct {
                      u64 bone_address;
                      u32 skin_offset;
                      u32 pad;
                    } skin{frame.bone_palette_address, static_cast<u32>(item.skin_offset), 0};
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 2 * sizeof(Mat4),
                                       sizeof(skin), &skin);
                  }
                  vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                  for (const GpuSubmesh& submesh : mesh->submeshes) {
                    if (submesh.blend) continue;
                    // Bind the material so masked casters alpha-test in the fragment.
                    VkDescriptorSet material = material_system_->set(submesh.material);
                    if (material != bound_material) {
                      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                              &material, 0, nullptr);
                      bound_material = material;
                    }
                    vkCmdDrawIndexed(cmd, submesh.index_count, 1, submesh.index_offset, 0, 0);
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
  VkBuffer cull_commands = gpu_cull_.command_buffer(cull_slot);
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
      const base::Vector<GpuSubmesh>& src =
          lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
      i32 vtx_off = lod == 0 ? 0 : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);

      u32 mesh_cmds = 0;
      u32 k = 0;
      for (const GpuSubmesh& submesh : mesh->submeshes) {
        if (!submesh.blend) {
          if (cmd_total >= GpuCull::kMaxCommands) break;
          const GpuSubmesh& s = k < src.size() ? src[k] : submesh;
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
       ms_occlude, cull_hiz, &ms_occ, &ms_planes](PassContext& ctx) {
        VkRenderingAttachmentInfo colors[3];
        colors[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[0].imageView = ctx.graph->image(normals).view;
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[1] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[1].imageView = ctx.graph->image(motion).view;
        colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[2] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[2].imageView = ctx.graph->image(depth_export).view;
        colors[2].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo depth_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_attachment.imageView = ctx.graph->image(depth).view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clearValue.depthStencil = {0.0f, 0};  // reversed z clears to far = 0

        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {render_width_, render_height_}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 3;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = &depth_attachment;
        vkCmdBeginRendering(ctx.cmd, &rendering);

        VkViewport viewport{0, 0, static_cast<f32>(render_width_),
                            static_cast<f32>(render_height_), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {render_width_, render_height_}};
        vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

        VkDescriptorSet globals_set = ctx.allocate_set(mesh_pipeline_->set_layout());
        VkDescriptorBufferInfo buffer_info{frame.globals.buffer, 0, sizeof(FrameGlobals)};
        VkWriteDescriptorSet writes[2];
        u32 nwrites = 1;
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = globals_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        VkDescriptorImageInfo hiz_info{};
        if (ms_active) {  // hi-z for the task-stage occlusion cull (real or fallback)
          hiz_info.imageView = ms_occlude ? ctx.graph->image(cull_hiz).view : ms_dummy_hiz_.view;
          hiz_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[1].dstSet = globals_set;
          writes[1].dstBinding = 2;
          writes[1].descriptorCount = 1;
          writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[1].pImageInfo = &hiz_info;
          nwrites = 2;
        }
        vkUpdateDescriptorSets(device_->device(), nwrites, writes, 0, nullptr);

        // Mesh-shader sub-pass: static opaque meshes, cluster-culled on the gpu.
        if (ms_active) {
          mesh_pipeline_->BindMeshPrepass(ctx.cmd, globals_set);
          VkDescriptorSet bound = VK_NULL_HANDLE;
          for (const DrawItem& item : view.draws) {
            const GpuMesh* mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend || !mesh->has_meshlets) continue;
            MeshShaderPush push{};
            push.model = item.transform;
            push.prev_model = item.prev_transform;
            push.meshlets_address = mesh->meshlets_address;
            push.meshlet_vertices_address = mesh->meshlet_vertices_address;
            push.meshlet_triangles_address = mesh->meshlet_triangles_address;
            push.vertices_address = mesh->vertices_address;
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
              VkDescriptorSet material = material_system_->set(submesh.material);
              if (material != bound) {
                mesh_pipeline_->BindMeshMaterial(ctx.cmd, material);
                bound = material;
              }
              push.meshlet_offset = submesh.meshlet_offset;
              push.meshlet_count = submesh.meshlet_count;
              mesh_pipeline_->DrawMeshlets(ctx.cmd, push);
            }
          }
        }

        // Raster sub-pass: skinned / non-meshlet meshes via gpu-culled indirect
        // draws. Meshes already drawn by the mesh shader are skipped but still
        // advance the cull index so it stays aligned with the cull build order.
        mesh_pipeline_->BindPrepass(ctx.cmd, globals_set);
        VkDescriptorSet bound_material = VK_NULL_HANDLE;
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
              mesh_pipeline_->SetPrepassSkinned(ctx.cmd, draw_skinned);
              skinned_bound = draw_skinned;
            }
            MeshPushConstants push{.model = item.transform, .prev_model = item.prev_transform};
            if (draw_skinned && item.skin_offset >= 0) {
              push.bone_address = frame.bone_palette_address;
              push.skin_offset = static_cast<u32>(item.skin_offset);
            }
            mesh_pipeline_->Draw(ctx.cmd, *mesh, push);
          }
          for (const GpuSubmesh& submesh : mesh->submeshes) {
            if (submesh.blend) continue;  // transparency owns its own depth
            if (cull_cmd_index >= cull_total_commands_) break;  // partial-mesh boundary
            if (!ms_handled) {
              VkDescriptorSet material = material_system_->set(submesh.material);
              if (material != bound_material) {
                mesh_pipeline_->BindMaterial(ctx.cmd, material);
                bound_material = material;
              }
              vkCmdDrawIndexedIndirect(ctx.cmd, cull_commands,
                                       cull_cmd_index * GpuCull::kCommandStride, 1,
                                       GpuCull::kCommandStride);
            }
            ++cull_cmd_index;
          }
        }
        vkCmdEndRendering(ctx.cmd);
      });

  // Snapshot this frame's depth for next frame's occlusion test.
  if (settings_.gpu_culling && settings_.gpu_occlusion) {
    gpu_cull_.CopyDepth(graph_, depth_export, cull_slot);
  }

  if (ddgi_active) {
    ddgi_->AddToGraph(graph_, *raytracing_, tlas_slot, view.camera.eye,
                      applied_sun_direction_, applied_sun_intensity_, applied_sun_color_,
                      frame_index_);
  }

  ResourceHandle ao = kInvalidResource;
  ResourceHandle sun_shadow = kInvalidResource;
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

  graph_.AddPass(
      "scene",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(scene_color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
        if (ao != kInvalidResource) builder.Read(ao, ResourceUsage::kSampledFragment);
        if (sun_shadow != kInvalidResource) builder.Read(sun_shadow, ResourceUsage::kSampledFragment);
        if (csm_active) builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
        if (ms_occlude) builder.Read(cull_hiz, ResourceUsage::kSampledTaskMesh);
      },
      [this, scene_color, motion, depth, ao, sun_shadow, tlas_slot, rt_shadows, use_rt_frag,
       ddgi_active, csm_active, shadow_slot, shadow_atlas, cull_commands, ms_active, ms_occlude,
       cull_hiz, &ms_occ, &ms_planes, &frame, &view](PassContext& ctx) {
        VkRenderingAttachmentInfo colors[2];
        colors[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[0].imageView = ctx.graph->image(scene_color).view;
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[0].clearValue.color = {{0.02f, 0.02f, 0.05f, 1.0f}};
        colors[1] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[1].imageView = ctx.graph->image(motion).view;
        colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // the prepass wrote motion
        colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo depth_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_attachment.imageView = ctx.graph->image(depth).view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // prepass depth, tested EQUAL
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {render_width_, render_height_}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 2;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = &depth_attachment;
        vkCmdBeginRendering(ctx.cmd, &rendering);

        VkViewport viewport{0, 0, static_cast<f32>(render_width_),
                            static_cast<f32>(render_height_), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {render_width_, render_height_}};
        vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

        VkDescriptorSet globals_set = ctx.allocate_set(mesh_pipeline_->set_layout());
        VkDescriptorBufferInfo buffer_info{frame.globals.buffer, 0, sizeof(FrameGlobals)};
        VkWriteDescriptorSet writes[3];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = globals_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        u32 write_count = 1;

        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        if (use_rt_frag) {
          tlas = raytracing_->tlas(tlas_slot);
          tlas_info.accelerationStructureCount = 1;
          tlas_info.pAccelerationStructures = &tlas;
          writes[write_count] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[write_count].pNext = &tlas_info;
          writes[write_count].dstSet = globals_set;
          writes[write_count].dstBinding = 1;
          writes[write_count].descriptorCount = 1;
          writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
          ++write_count;
        }
        VkDescriptorImageInfo hiz_info{};
        if (ms_active) {  // hi-z for the task-stage occlusion cull (real or fallback)
          hiz_info.imageView = ms_occlude ? ctx.graph->image(cull_hiz).view : ms_dummy_hiz_.view;
          hiz_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          writes[write_count] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[write_count].dstSet = globals_set;
          writes[write_count].dstBinding = 2;
          writes[write_count].descriptorCount = 1;
          writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[write_count].pImageInfo = &hiz_info;
          ++write_count;
        }
        vkUpdateDescriptorSets(device_->device(), write_count, writes, 0, nullptr);

        VkDescriptorSet env_set = ctx.allocate_set(environment_->env_set_layout());
        VkImageView ao_view =
            ao != kInvalidResource ? ctx.graph->image(ao).view : VK_NULL_HANDLE;
        VkImageView sun_shadow_view =
            sun_shadow != kInvalidResource ? ctx.graph->image(sun_shadow).view : VK_NULL_HANDLE;
        EnvironmentSystem::DdgiBinding ddgi_binding;
        if (ddgi_active) ddgi_binding = ddgi_->binding(frame_index_);
        environment_->WriteEnvSet(
            env_set, ao_view, ddgi_active ? &ddgi_binding : nullptr,
            csm_active ? ctx.graph->image(shadow_atlas).view : VK_NULL_HANDLE,
            csm_active ? shadow_.cascade_buffer(shadow_slot) : VK_NULL_HANDLE,
            shadow_.cascade_buffer_size(), VK_NULL_HANDLE, sun_shadow_view, frame.lights.buffer,
            frame.lights.size);

        VkDescriptorSet bindless_set = bindless_ ? bindless_->set() : VK_NULL_HANDLE;

        // Mesh-shader sub-pass: static opaque meshes, finest lod, cluster-culled.
        if (ms_active) {
          mesh_pipeline_->BindMeshScene(ctx.cmd, globals_set, env_set, bindless_set, use_rt_frag);
          VkDescriptorSet bound = VK_NULL_HANDLE;
          for (const DrawItem& item : view.draws) {
            const GpuMesh* mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend || !mesh->has_meshlets) continue;
            MeshShaderPush push{};
            push.model = item.transform;
            push.prev_model = item.prev_transform;
            push.meshlets_address = mesh->meshlets_address;
            push.meshlet_vertices_address = mesh->meshlet_vertices_address;
            push.meshlet_triangles_address = mesh->meshlet_triangles_address;
            push.vertices_address = mesh->vertices_address;
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
              VkDescriptorSet material = material_system_->set(submesh.material);
              if (material != bound) {
                mesh_pipeline_->BindMeshMaterial(ctx.cmd, material);
                bound = material;
              }
              push.meshlet_offset = submesh.meshlet_offset;
              push.meshlet_count = submesh.meshlet_count;
              mesh_pipeline_->DrawMeshlets(ctx.cmd, push);
            }
          }
        }

        mesh_pipeline_->Bind(ctx.cmd, globals_set, env_set, bindless_set, use_rt_frag,
                             settings_.wireframe);
        VkDescriptorSet bound_material = VK_NULL_HANDLE;
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
              mesh_pipeline_->SetSkinned(ctx.cmd, draw_skinned, use_rt_frag, settings_.wireframe);
              skinned_bound = draw_skinned;
            }
            MeshPushConstants push{.model = item.transform, .prev_model = item.prev_transform};
            if (draw_skinned && item.skin_offset >= 0) {
              push.bone_address = frame.bone_palette_address;
              push.skin_offset = static_cast<u32>(item.skin_offset);
            }
            push.tint_packed = item.tint;  // faction/team colour for skinned actors
            mesh_pipeline_->Draw(ctx.cmd, *mesh, push);
          }
          for (const GpuSubmesh& submesh : mesh->submeshes) {
            if (submesh.blend) continue;
            if (cull_cmd_index >= cull_total_commands_) break;  // partial-mesh boundary
            if (!ms_handled) {
              VkDescriptorSet material = material_system_->set(submesh.material);
              if (material != bound_material) {
                mesh_pipeline_->BindMaterial(ctx.cmd, material);
                bound_material = material;
              }
              vkCmdDrawIndexedIndirect(ctx.cmd, cull_commands,
                                       cull_cmd_index * GpuCull::kCommandStride, 1,
                                       GpuCull::kCommandStride);
            }
            ++cull_cmd_index;
          }
        }
        if (skinned_bound) {
          mesh_pipeline_->SetSkinned(ctx.cmd, false, use_rt_frag, settings_.wireframe);
        }
        if (settings_.sky) environment_->DrawSky(ctx.cmd, globals_set);
        vkCmdEndRendering(ctx.cmd);
      });

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
    lit = composite;
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
          if (csm_active) builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
        },
        [this, composite, motion, depth, opaque_color, opaque_depth, sun_shadow, tlas_slot,
         rt_shadows, use_rt_frag, ddgi_active, water_pipeline_active, csm_active, shadow_slot,
         shadow_atlas, transparent = std::move(transparent), &frame, &view](PassContext& ctx) {
          VkRenderingAttachmentInfo colors[2];
          colors[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          colors[0].imageView = ctx.graph->image(composite).view;
          colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          colors[1] = colors[0];
          colors[1].imageView = ctx.graph->image(motion).view;

          VkRenderingAttachmentInfo depth_attachment{
              .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          depth_attachment.imageView = ctx.graph->image(depth).view;
          depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
          depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

          VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
          rendering.renderArea = {{0, 0}, {render_width_, render_height_}};
          rendering.layerCount = 1;
          rendering.colorAttachmentCount = 2;
          rendering.pColorAttachments = colors;
          rendering.pDepthAttachment = &depth_attachment;
          vkCmdBeginRendering(ctx.cmd, &rendering);

          VkViewport viewport{0, 0, static_cast<f32>(render_width_),
                              static_cast<f32>(render_height_), 0.0f, 1.0f};
          VkRect2D scissor{{0, 0}, {render_width_, render_height_}};
          vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
          vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

          VkDescriptorSet globals_set = ctx.allocate_set(mesh_pipeline_->set_layout());
          VkDescriptorBufferInfo buffer_info{frame.globals.buffer, 0, sizeof(FrameGlobals)};
          VkWriteDescriptorSet writes[2];
          writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[0].dstSet = globals_set;
          writes[0].dstBinding = 0;
          writes[0].descriptorCount = 1;
          writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          writes[0].pBufferInfo = &buffer_info;
          u32 write_count = 1;
          VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
          VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
          if (rt_available_ && raytracing_) {
            tlas = raytracing_->tlas(tlas_slot);
            if (tlas != VK_NULL_HANDLE) {
              tlas_info.accelerationStructureCount = 1;
              tlas_info.pAccelerationStructures = &tlas;
              writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
              writes[1].pNext = &tlas_info;
              writes[1].dstSet = globals_set;
              writes[1].dstBinding = 1;
              writes[1].descriptorCount = 1;
              writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
              write_count = 2;
            }
          }
          vkUpdateDescriptorSets(device_->device(), write_count, writes, 0, nullptr);

          VkDescriptorSet env_set = ctx.allocate_set(environment_->env_set_layout());
          EnvironmentSystem::DdgiBinding ddgi_binding;
          if (ddgi_active) ddgi_binding = ddgi_->binding(frame_index_);
          VkImageView sun_shadow_view =
              sun_shadow != kInvalidResource ? ctx.graph->image(sun_shadow).view : VK_NULL_HANDLE;
          environment_->WriteEnvSet(
              env_set, VK_NULL_HANDLE, ddgi_active ? &ddgi_binding : nullptr,
              csm_active ? ctx.graph->image(shadow_atlas).view : VK_NULL_HANDLE,
              csm_active ? shadow_.cascade_buffer(shadow_slot) : VK_NULL_HANDLE,
              shadow_.cascade_buffer_size(), ctx.graph->image(opaque_color).view, sun_shadow_view,
              frame.lights.buffer, frame.lights.size);

          enum class Mode { kNone, kWater, kBlend };
          Mode mode = Mode::kNone;
          VkDescriptorSet bound_material = VK_NULL_HANDLE;
          const DrawItem* bound_item = nullptr;
          for (const TransparentDraw& draw : transparent) {
            const GpuMesh* mesh = meshes_.find(draw.item->mesh);
            if (!mesh) continue;
            bool as_water = draw.submesh->water && water_pipeline_active;
  
            Mode wanted = as_water ? Mode::kWater : Mode::kBlend;
            if (mode != wanted) {
              if (as_water) {
                water_->Bind(ctx, globals_set, env_set, bindless_->set(), opaque_color,
                             opaque_depth);
              } else {
                VkDescriptorSet bindless_set = bindless_ ? bindless_->set() : VK_NULL_HANDLE;
                mesh_pipeline_->BindBlend(ctx.cmd, globals_set, env_set, bindless_set,
                                          use_rt_frag);
              }
              mode = wanted;
              bound_material = VK_NULL_HANDLE;
              bound_item = nullptr;
            }
            if (draw.item != bound_item) {
              MeshPushConstants push{.model = draw.item->transform,
                                     .prev_model = draw.item->prev_transform};
              if (as_water) {
                vkCmdPushConstants(ctx.cmd, water_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &mesh->vertices.buffer, &offset);
                vkCmdBindIndexBuffer(ctx.cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
              } else {
                mesh_pipeline_->Draw(ctx.cmd, *mesh, push);
              }
              bound_item = draw.item;
            }
            VkDescriptorSet material = material_system_->set(draw.submesh->material);
            if (material != bound_material) {
              if (as_water) {
                water_->BindMaterial(ctx.cmd, material);
              } else {
                mesh_pipeline_->BindMaterial(ctx.cmd, material);
              }
              bound_material = material;
            }
            mesh_pipeline_->DrawSubmesh(ctx.cmd, *draw.submesh);
          }
          vkCmdEndRendering(ctx.cmd);
        });
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
  if (settings_.aerial_perspective > 0.0f && !path_trace) {
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
  if (settings_.clouds && !path_trace) {
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
    lit = wboit_.AddToGraph(graph_, lit, depth, view.oit, view_proj, applied_sun_direction_,
                            sun_col, std::max(settings_.ambient, 0.12f), render_width_,
                            render_height_);
  }

  // Lit billboard particles blend over the resolved scene, faded against the
  // prepass depth, before temporal reconstruction. Either a cpu-uploaded set or
  // the gpu-simulated fountain.
  if (!view.particles.empty() || view.gpu_particle_count > 0) {
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
    if (view.gpu_particle_count > 0) {
      ParticleSystem::Sim sim;
      sim.emitter[0] = view.gpu_particle_emitter.x;
      sim.emitter[1] = view.gpu_particle_emitter.y;
      sim.emitter[2] = view.gpu_particle_emitter.z;
      sim.dt = view.frame_delta_seconds;
      sim.count = view.gpu_particle_count;
      particles_.SimulateAndDraw(graph_, lit, depth_export, motion, sim, pf, frame_index_ % 2);
    } else {
      particles_.AddToGraph(graph_, lit, depth_export, motion, view.particles, pf, frame_index_ % 2);
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
              ctx.cmd, ctx.graph->image(lit).view, {render_width_, render_height_}, view_proj,
              [this, &view](VkCommandBuffer cmd, VkPipelineLayout layout) {
                for (const DrawItem& item : view.draws) {
                  const GpuMesh* mesh = meshes_.find(item.mesh);
                  if (!mesh || mesh->indices.buffer == VK_NULL_HANDLE) continue;
                  vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(Mat4),
                                     sizeof(Mat4), &item.transform);
                  VkDeviceSize offset = 0;
                  vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices.buffer, &offset);
                  vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                  for (const GpuSubmesh& submesh : mesh->submeshes) {
                    vkCmdDrawIndexed(cmd, submesh.index_count, 1, submesh.index_offset, 0, 0);
                  }
                }
              });
        });
  }
  }  // end raster path

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
      hdr_readback_ = device_->CreateBuffer(need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    hdr_width_ = post_width;
    hdr_height_ = post_height;
    hdr_pending_ = hdr_readback_.mapped != nullptr;
    if (hdr_pending_) {
      VkBuffer dst = hdr_readback_.buffer;
      graph_.AddPass(
          "hdr_capture",
          [&](RenderGraph::PassBuilder& builder) {
            builder.Read(post_input, ResourceUsage::kSampledCompute);
          },
          [this, post_input, dst, post_width, post_height](PassContext& ctx) {
            VkDescriptorSet set = ctx.allocate_set(hdr_set_layout_);
            VkDescriptorBufferInfo binfo{dst, 0, VK_WHOLE_SIZE};
            VkDescriptorImageInfo iinfo{VK_NULL_HANDLE, ctx.graph->image(post_input).view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w[2];
            w[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[0].dstSet = set;
            w[0].dstBinding = 0;
            w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[0].pBufferInfo = &binfo;
            w[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[1].dstSet = set;
            w[1].dstBinding = 1;
            w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[1].pImageInfo = &iinfo;
            vkUpdateDescriptorSets(ctx.device->device(), 2, w, 0, nullptr);

            u32 push[2] = {post_width, post_height};
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hdr_pipeline_);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hdr_layout_, 0, 1, &set,
                                    0, nullptr);
            vkCmdPushConstants(ctx.cmd, hdr_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               push);
            vkCmdDispatch(ctx.cmd, (post_width + 7) / 8, (post_height + 7) / 8, 1);

            VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);
          });
    }
  }

  exposure_.AddToGraph(graph_, post_input, post_width, post_height, view.frame_delta_seconds);
  ResourceHandle bloom = kInvalidResource;
  if (settings_.bloom) {
    bloom = bloom_.AddToGraph(graph_, post_input, post_width, post_height);
  }

  GpuImage backbuffer_image;
  backbuffer_image.image = swapchain_->image(image_index);
  backbuffer_image.view = swapchain_->view(image_index);
  backbuffer_image.format = swapchain_->format();
  backbuffer_image.extent = swapchain_->extent();
  ResourceHandle backbuffer = graph_.ImportBackbuffer(backbuffer_image);

  post_->SetGrade(settings_.color_grade);  // rebakes the lut only when it changes
  PostPass::Params post_params{static_cast<u32>(settings_.tonemap), settings_.bloom_intensity,
                               bloom != kInvalidResource ? 1u : 0u,
                               settings_.color_grade != ColorGrade::kNeutral ? 1u : 0u};
  graph_.AddPass(
      "post",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(post_input, ResourceUsage::kSampledFragment);
        if (bloom != kInvalidResource) builder.Read(bloom, ResourceUsage::kSampledFragment);
        builder.Write(backbuffer, ResourceUsage::kColorAttachment);
      },
      [this, post_input, bloom, backbuffer, post_params](PassContext& ctx) {
        VkImageView bloom_view = bloom != kInvalidResource ? ctx.graph->image(bloom).view
                                                           : ctx.graph->image(post_input).view;
        post_->Record(ctx, ctx.graph->image(post_input).view, bloom_view,
                      exposure_.exposure_buffer(), exposure_.exposure_buffer_size(),
                      ctx.graph->image(backbuffer).view, ctx.graph->image(backbuffer).extent,
                      post_params);
      });

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
          VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          color.imageView = ctx.graph->image(backbuffer).view;
          color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

          VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
          rendering.renderArea = {{0, 0}, ctx.graph->image(backbuffer).extent};
          rendering.layerCount = 1;
          rendering.colorAttachmentCount = 1;
          rendering.pColorAttachments = &color;
          // Hand the blurred backdrop to the UI before it records (the closure
          // reads view.blur_source); null when blur is not in play this frame.
          view.blur_source =
              ui_frost != kInvalidResource ? ctx.graph->image(ui_frost).view : VK_NULL_HANDLE;
          view.blur_sampler = ui_frost != kInvalidResource ? ui_blur_->sampler() : VK_NULL_HANDLE;
          vkCmdBeginRendering(ctx.cmd, &rendering);
          if (view.hud_draw) view.hud_draw(ctx.cmd);
          if (view.ui_draw) view.ui_draw(ctx.cmd);
          vkCmdEndRendering(ctx.cmd);
        });
  }
}

bool Renderer::CreateFrameResources() {
  for (FrameResources& frame : frames_) {
    VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = device_->graphics_family();
    if (vkCreateCommandPool(device_->device(), &pool_info, nullptr, &frame.pool) != VK_SUCCESS) {
      return false;
    }

    VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = frame.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_->device(), &alloc, &frame.cmd);

    VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(device_->device(), &semaphore_info, nullptr, &frame.image_available) !=
            VK_SUCCESS ||
        vkCreateFence(device_->device(), &fence_info, nullptr, &frame.in_flight) != VK_SUCCESS) {
      return false;
    }

    // SAMPLED_IMAGE / SAMPLER / UNIFORM_BUFFER_DYNAMIC are for NRD's many per
    // frame dispatch sets; harmless reservations otherwise. The acceleration
    // structure size must stay last so it can be dropped without the extension.
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 24},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 128},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4},
    };
    VkDescriptorPoolCreateInfo descriptor_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_info.maxSets = 256;
    // The acceleration structure pool size is only legal with the extension
    // enabled, drop it otherwise.
    descriptor_info.poolSizeCount = device_->caps().raytracing ? 8 : 7;
    descriptor_info.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_->device(), &descriptor_info, nullptr,
                               &frame.descriptor_pool) != VK_SUCCESS) {
      return false;
    }

    frame.globals = device_->CreateBuffer(sizeof(FrameGlobals),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    if (!frame.globals.mapped) return false;

    // Bone palette: host visible, read in the skinned vertex shader through its
    // device address (no descriptor binding). Column-major 4x4 per bone.
    frame.bone_palette = device_->CreateBuffer(
        static_cast<u64>(kMaxFrameBones) * sizeof(Mat4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    if (!frame.bone_palette.mapped) return false;
    VkBufferDeviceAddressInfo address_info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = frame.bone_palette.buffer;
    frame.bone_palette_address = vkGetBufferDeviceAddress(device_->device(), &address_info);

    frame.lights = device_->CreateBuffer(static_cast<u64>(kMaxFrameLights) * sizeof(PointLight),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!frame.lights.mapped) return false;
  }
  return true;
}

void Renderer::DestroyFrameResources() {
  for (FrameResources& frame : frames_) {
    if (frame.globals.buffer) device_->DestroyBuffer(frame.globals);
    if (frame.bone_palette.buffer) device_->DestroyBuffer(frame.bone_palette);
    if (frame.lights.buffer) device_->DestroyBuffer(frame.lights);
    if (frame.descriptor_pool) {
      vkDestroyDescriptorPool(device_->device(), frame.descriptor_pool, nullptr);
    }
    if (frame.in_flight) vkDestroyFence(device_->device(), frame.in_flight, nullptr);
    if (frame.image_available) vkDestroySemaphore(device_->device(), frame.image_available, nullptr);
    if (frame.pool) vkDestroyCommandPool(device_->device(), frame.pool, nullptr);
    frame = {};
  }
  DestroyRenderFinishedSemaphores();
}

bool Renderer::CreateRenderFinishedSemaphores() {
  render_finished_.resize(swapchain_->image_count());
  for (VkSemaphore& semaphore : render_finished_) {
    VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device_->device(), &info, nullptr, &semaphore) != VK_SUCCESS) {
      return false;
    }
  }
  return true;
}

void Renderer::DestroyRenderFinishedSemaphores() {
  for (VkSemaphore semaphore : render_finished_) {
    if (semaphore) vkDestroySemaphore(device_->device(), semaphore, nullptr);
  }
  render_finished_.clear();
}

void Renderer::RecreateSwapchain() {
  u32 width = window_->width();
  u32 height = window_->height();
  if (width == 0 || height == 0) return;  // minimized
  device_->WaitIdle();
  swapchain_.reset();
  swapchain_ = Swapchain::Create(*device_, width, height, settings_.vsync);
  if (!swapchain_) return;
  DestroyRenderFinishedSemaphores();
  if (!CreateRenderFinishedSemaphores()) return;
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
  UpdateRenderResolution();
  transient_pool_->Clear();
  taa_.Resize(*device_, {render_width_, render_height_});
  ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
  path_tracer_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_) rtao_.Resize(*device_, {render_width_, render_height_});
#if defined(RECREATION_HAS_NRD)
  if (rt_available_) shadow_trace_.Resize(*device_, {render_width_, render_height_});
#endif
  has_prev_frame_ = false;
}

void Renderer::DestroySurface() {
  if (!device_ || device_->is_stub()) return;
  device_->WaitIdle();
  swapchain_.reset();
  DestroyRenderFinishedSemaphores();
  device_->DestroySurface();
}

void Renderer::RecreateSurface() {
  if (!device_ || device_->is_stub() || !window_) return;
  if (!device_->RecreateSurface(*window_)) return;
  RecreateSwapchain();  // rebuilds the swapchain, semaphores and sized targets
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
      if (kv.value.skinning.buffer) device_->DestroyBuffer(kv.value.skinning);
      if (kv.value.meshlets.buffer) device_->DestroyBuffer(kv.value.meshlets);
      if (kv.value.meshlet_vertices.buffer) device_->DestroyBuffer(kv.value.meshlet_vertices);
      if (kv.value.meshlet_triangles.buffer) device_->DestroyBuffer(kv.value.meshlet_triangles);
    }
    meshes_.clear();
    taa_.Destroy(*device_);
    ssao_.Destroy(*device_);
    ssr_.Destroy(*device_);
    ssgi_.Destroy(*device_);
    if (hdr_pipeline_) vkDestroyPipeline(device_->device(), hdr_pipeline_, nullptr);
    if (hdr_layout_) vkDestroyPipelineLayout(device_->device(), hdr_layout_, nullptr);
    if (hdr_set_layout_) vkDestroyDescriptorSetLayout(device_->device(), hdr_set_layout_, nullptr);
    device_->DestroyBuffer(hdr_readback_);
    shadow_.Destroy(*device_);
    particles_.Destroy(*device_);
    gaussians_.Destroy(*device_);
    fur_.Destroy(*device_);
    wboit_.Destroy(*device_);
    overdraw_.Destroy(*device_);
    gpu_cull_.Destroy(*device_);
    meshlet_.Destroy(*device_);
    if (ms_dummy_hiz_.image) device_->DestroyImage(ms_dummy_hiz_);
    if (rt_available_) rtao_.Destroy(*device_);
#if defined(RECREATION_HAS_NRD)
    if (rt_available_) nrd_.Destroy(*device_);
    if (rt_available_) shadow_trace_.Destroy(*device_);
#endif
    bloom_.Destroy(*device_);
    exposure_.Destroy(*device_);
    profiler_.Shutdown();
    path_tracer_.Destroy(*device_);
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
  ui_blur_.reset();  // holds a Device& + Vk handles; destroy before device_/instance
  mesh_pipeline_.reset();
  swapchain_.reset();
  upscaler_.reset();
  raytracing_.reset();
  device_.reset();
}

const DeviceCaps* Renderer::caps() const { return device_ ? &device_->caps() : nullptr; }

VkFormat Renderer::swapchain_format() const {
  return swapchain_ ? swapchain_->format() : VK_FORMAT_UNDEFINED;
}

u32 Renderer::swapchain_image_count() const {
  return swapchain_ ? swapchain_->image_count() : 0;
}

}  // namespace rec::render

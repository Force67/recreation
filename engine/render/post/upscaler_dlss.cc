#include "render/post/upscaler.h"

#include <cstdlib>
#include <string>

#include <base/option.h>

#include "core/log.h"
#include "render/rhi/device.h"  // pulls volk (VK_NO_PROTOTYPES) before the ngx vk header

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>

namespace rec::render {
namespace {

// Arbitrary application id for the CUSTOM engine path; NGX validates it is a
// well formed UUID (8-4-4-4-12 hex), it does not need to be registered.
constexpr const char* kProjectId = "8d4a1f60-3c2e-4b8a-9f17-c4035c19df04";

// RECREATION_DLSS_LIB_DIR overrides the baked-in snippet dir; was read straight
// from the environment.
base::Option<const char*> DlssLibDir{"dlss.lib.dir", nullptr, "RECREATION_DLSS_LIB_DIR"};

void NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string line(message ? message : "");
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (!line.empty()) REC_WARN("ngx: {}", line);
}

// The engine derives render resolution from a fixed per-axis ratio (see
// UpscalerScale), so map that ratio back onto the closest DLSS preset.
NVSDK_NGX_PerfQuality_Value QualityFromRatio(u32 render_width, u32 output_width) {
  f32 ratio = static_cast<f32>(output_width) / static_cast<f32>(render_width);
  if (ratio < 1.15f) return NVSDK_NGX_PerfQuality_Value_DLAA;
  if (ratio < 1.6f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  if (ratio < 1.85f) return NVSDK_NGX_PerfQuality_Value_Balanced;
  return NVSDK_NGX_PerfQuality_Value_MaxPerf;
}

std::wstring ToWide(const char* s) {
  std::wstring w;
  for (; s && *s; ++s) w.push_back(static_cast<wchar_t>(*s));
  return w;
}

class DlssUpscaler final : public Upscaler {
 public:
  explicit DlssUpscaler(Device& device) : device_(device) {}
  ~DlssUpscaler() override { Destroy(); }

  bool Initialize(const UpscalerDesc& desc) override {
    desc_ = desc;

    // PathListInfo tells NGX where the DLSS inference snippet (.so) lives; the
    // SDK lib dir is baked in at build time by third_party/dlss.cmake, with an
    // env override so a driver-bundled or dev snippet can be pointed at without
    // a rebuild.
    const char* lib_dir = DlssLibDir.get();
    snippet_dir_ = ToWide(lib_dir && *lib_dir ? lib_dir : RECREATION_DLSS_LIB_DIR);
    snippet_path_ = snippet_dir_.c_str();
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = &snippet_path_;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.LoggingCallback = NgxLog;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", L"/tmp", device_.instance(),
        device_.physical_device(), device_.device(), vkGetInstanceProcAddr, vkGetDeviceProcAddr,
        &common, NVSDK_NGX_Version_API);
    if (r != NVSDK_NGX_Result_Success) {
      REC_ERROR("dlss: ngx init failed ({:#x})", static_cast<u32>(r));
      return false;
    }
    ngx_initialized_ = true;

    if (NVSDK_NGX_VULKAN_GetCapabilityParameters(&params_) != NVSDK_NGX_Result_Success || !params_) {
      REC_ERROR("dlss: capability parameters unavailable");
      return false;
    }
    // The availability flag is populated by NGX's updater/DRS probe, which is
    // skipped on a self-contained Linux setup (no writable models dir, no
    // updater), so it reads 0 even when the snippet loads. Treat a stale driver
    // as the only hard stop and otherwise let feature creation be the real test.
    int supported = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                             &needs_driver);
    if (needs_driver) {
      unsigned major = 0, minor = 0;
      NVSDK_NGX_Parameter_GetUI(params_, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
                                &major);
      NVSDK_NGX_Parameter_GetUI(params_, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
                                &minor);
      REC_ERROR("dlss: driver too old, needs >= {}.{}", major, minor);
      return false;
    }
    if (!supported) REC_WARN("dlss: availability probe inconclusive, trying anyway");

    // Feature creation records into a command buffer; reuse the device's one
    // shot submit, the same way the fsr3 backend primes its shared resources.
    bool created = false;
    device_.ImmediateSubmit([&](VkCommandBuffer cmd) {
      NVSDK_NGX_DLSS_Create_Params create{};
      create.Feature.InWidth = desc.render_width;
      create.Feature.InHeight = desc.render_height;
      create.Feature.InTargetWidth = desc.output_width;
      create.Feature.InTargetHeight = desc.output_height;
      create.Feature.InPerfQualityValue = QualityFromRatio(desc.render_width, desc.output_width);
      // Linear HDR, render-resolution motion vectors, reversed-z depth, and let
      // DLSS estimate exposure itself (the engine's exposure runs post-upscale).
      create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
      NVSDK_NGX_Result cr = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1, 1, &handle_, params_, &create);
      created = cr == NVSDK_NGX_Result_Success;
      if (!created) REC_ERROR("dlss: feature creation failed ({:#x})", static_cast<u32>(cr));
    });
    if (!created) return false;

    REC_INFO("dlss upscaler ready: {}x{} -> {}x{}", desc.render_width, desc.render_height,
             desc.output_width, desc.output_height);
    return true;
  }

  ResourceHandle AddToGraph(RenderGraph& graph, const UpscalerInputs& inputs) override {
    ResourceHandle output = graph.CreateTexture({.name = "dlss_output",
                                                 .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                                 .width = desc_.output_width,
                                                 .height = desc_.output_height});
    graph.AddPass(
        "dlss_upscale",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Read(inputs.color, ResourceUsage::kSampledCompute);
          builder.Read(inputs.depth, ResourceUsage::kSampledCompute);
          builder.Read(inputs.motion_vectors, ResourceUsage::kSampledCompute);
          builder.Write(output, ResourceUsage::kStorageWrite);
        },
        [this, inputs, output](PassContext& ctx) { Dispatch(ctx, inputs, output); });
    return output;
  }

  UpscalerKind kind() const override { return UpscalerKind::kDlss; }

 private:
  void Dispatch(PassContext& ctx, const UpscalerInputs& inputs, ResourceHandle output) {
    const GpuImage& color = ctx.graph->image(inputs.color);
    const GpuImage& depth = ctx.graph->image(inputs.depth);
    const GpuImage& motion = ctx.graph->image(inputs.motion_vectors);
    const GpuImage& out = ctx.graph->image(output);

    // depth_export is an R32_SFLOAT color image, so every input uses the color
    // aspect; the graph leaves reads in SHADER_READ_ONLY and the output in
    // GENERAL, which is what NGX expects.
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto wrap = [&](const GpuImage& image, bool read_write) {
      return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format,
                                                    image.extent.width, image.extent.height,
                                                    read_write);
    };
    NVSDK_NGX_Resource_VK color_res = wrap(color, false);
    NVSDK_NGX_Resource_VK depth_res = wrap(depth, false);
    NVSDK_NGX_Resource_VK motion_res = wrap(motion, false);
    NVSDK_NGX_Resource_VK output_res = wrap(out, true);

    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color_res;
    eval.Feature.pInOutput = &output_res;
    eval.pInDepth = &depth_res;
    eval.pInMotionVectors = &motion_res;
    eval.InJitterOffsetX = inputs.jitter_x;
    eval.InJitterOffsetY = inputs.jitter_y;
    eval.InRenderSubrectDimensions = {desc_.render_width, desc_.render_height};
    eval.InReset = inputs.reset_history ? 1 : 0;
    // The motion target stores uv-space current->previous offsets; scaling by
    // the render size yields the pixel-space vectors DLSS expects.
    eval.InMVScaleX = static_cast<f32>(desc_.render_width);
    eval.InMVScaleY = static_cast<f32>(desc_.render_height);

    NVSDK_NGX_Result er = NGX_VULKAN_EVALUATE_DLSS_EXT(ctx.cmd, handle_, params_, &eval);
    if (er != NVSDK_NGX_Result_Success) {
      REC_ERROR("dlss: evaluate failed ({:#x})", static_cast<u32>(er));
    }
  }

  void Destroy() {
    // The renderer waits for device idle before destroying upscalers.
    if (handle_) {
      NVSDK_NGX_VULKAN_ReleaseFeature(handle_);
      handle_ = nullptr;
    }
    if (params_) {
      NVSDK_NGX_VULKAN_DestroyParameters(params_);
      params_ = nullptr;
    }
    if (ngx_initialized_) {
      NVSDK_NGX_VULKAN_Shutdown1(device_.device());
      ngx_initialized_ = false;
    }
  }

  Device& device_;
  UpscalerDesc desc_;
  std::wstring snippet_dir_;
  const wchar_t* snippet_path_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* handle_ = nullptr;
  bool ngx_initialized_ = false;
};

}  // namespace

std::unique_ptr<Upscaler> CreateDlssUpscaler(const UpscalerDesc& desc, Device& device) {
  auto upscaler = std::make_unique<DlssUpscaler>(device);
  if (!upscaler->Initialize(desc)) return nullptr;
  return upscaler;
}

}  // namespace rec::render

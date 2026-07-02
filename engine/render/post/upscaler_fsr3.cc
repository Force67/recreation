#include "render/post/upscaler.h"

#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "render/rhi/device.h"
// Vulkan escape hatch: the FFX backend speaks raw Vulkan. Also pulls volk
// (VK_NO_PROTOTYPES) before the ffx vk header.
#include "render/rhi/vulkan_interop.h"

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

namespace rec::render {
namespace {

Format ToFormat(FfxSurfaceFormat format) {
  switch (format) {
    case FFX_SURFACE_FORMAT_R32_FLOAT: return Format::kR32Float;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return Format::kRG16Float;
    case FFX_SURFACE_FORMAT_R32_UINT: return Format::kR32Uint;
    default: return Format::kUnknown;
  }
}

// FfxResourceDescription for an engine image handed to the dispatch. The
// backend creates its own views from the raw VkImage, the depth usage flag
// makes it pick the depth aspect.
FfxResourceDescription DescribeImage(const GpuImage& image, FfxResourceUsage usage) {
  FfxResourceDescription desc{};
  desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  desc.format = ffxGetSurfaceFormatVK(GetVkFormat(image.format));
  desc.width = image.extent.width;
  desc.height = image.extent.height;
  desc.depth = 1;
  desc.mipCount = 1;
  desc.flags = FFX_RESOURCE_FLAGS_NONE;
  desc.usage = image.format == Format::kD32Float
                   ? static_cast<FfxResourceUsage>(usage | FFX_RESOURCE_USAGE_DEPTHTARGET)
                   : usage;
  return desc;
}

// The backend resolves its function table with the KHR/EXT names of
// functions that are core in 1.3; drivers return null for aliases of
// extensions the device did not enable, so retry with the core name.
PFN_vkVoidFunction DeviceProcAddr(VkDevice device, const char* name) {
  PFN_vkVoidFunction fn = vkGetDeviceProcAddr(device, name);
  if (fn) return fn;
  size_t len = std::strlen(name);
  if (len > 3 && (std::strcmp(name + len - 3, "KHR") == 0 ||
                  std::strcmp(name + len - 3, "EXT") == 0)) {
    std::string core(name, len - 3);
    fn = vkGetDeviceProcAddr(device, core.c_str());
  }
  return fn;
}

void MessageCallback(FfxMsgType type, const wchar_t* message) {
  std::string narrow;
  for (const wchar_t* c = message; c && *c; ++c) {
    narrow.push_back(*c < 128 ? static_cast<char>(*c) : '?');
  }
  if (type == FFX_MESSAGE_TYPE_ERROR) {
    REC_ERROR("fsr3: {}", narrow);
  } else {
    REC_WARN("fsr3: {}", narrow);
  }
}

class Fsr3Upscaler final : public Upscaler {
 public:
  explicit Fsr3Upscaler(Device& device) : device_(device) {}
  ~Fsr3Upscaler() override { Destroy(); }

  bool Initialize(const UpscalerDesc& desc) override {
    desc_ = desc;

    VulkanHandles h = GetVulkanHandles(device_);
    if (h.device == VK_NULL_HANDLE) {
      REC_WARN("fsr3: requires the vulkan backend, upscaler unavailable");
      return false;
    }

    VkDeviceContext device_context{h.device, h.physical_device, DeviceProcAddr};
    FfxDevice ffx_device = ffxGetDeviceVK(&device_context);
    scratch_size_ = ffxGetScratchMemorySizeVK(h.physical_device,
                                              FFX_FSR3UPSCALER_CONTEXT_COUNT);
    scratch_ = std::calloc(1, scratch_size_);
    if (!scratch_) return false;
    FfxErrorCode err = ffxGetInterfaceVK(&interface_, ffx_device, scratch_, scratch_size_,
                                         FFX_FSR3UPSCALER_CONTEXT_COUNT);
    if (err != FFX_OK) {
      REC_ERROR("fsr3: backend interface creation failed ({})", static_cast<int>(err));
      return false;
    }

    // The engine renders linear HDR with reversed-infinite depth and no
    // manual exposure, which maps exactly onto these flags.
    FfxFsr3UpscalerContextDescription context_desc{};
    context_desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                         FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED |
                         FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE |
                         FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE |
                         FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    context_desc.maxRenderSize = {desc.render_width, desc.render_height};
    context_desc.maxUpscaleSize = {desc.output_width, desc.output_height};
    context_desc.fpMessage = MessageCallback;
    context_desc.backendInterface = interface_;
    err = ffxFsr3UpscalerContextCreate(&context_, &context_desc);
    if (err != FFX_OK) {
      REC_ERROR("fsr3: context creation failed ({})", static_cast<int>(err));
      return false;
    }
    context_valid_ = true;

    if (!CreateSharedResources()) return false;

    REC_INFO("fsr3 upscaler ready: {}x{} -> {}x{}", desc.render_width, desc.render_height,
             desc.output_width, desc.output_height);
    return true;
  }

  ResourceHandle AddToGraph(RenderGraph& graph, const UpscalerInputs& inputs) override {
    ResourceHandle output = graph.CreateTexture({.name = "fsr3_output",
                                                 .format = Format::kRGBA16Float,
                                                 .width = desc_.output_width,
                                                 .height = desc_.output_height});
    graph.AddPass(
        "fsr3_upscale",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Read(inputs.color, ResourceUsage::kSampledCompute);
          builder.Read(inputs.depth, ResourceUsage::kSampledCompute);
          builder.Read(inputs.motion_vectors, ResourceUsage::kSampledCompute);
          builder.Write(output, ResourceUsage::kStorageWrite);
        },
        [this, inputs, output](PassContext& ctx) { Dispatch(ctx, inputs, output); });
    return output;
  }

  UpscalerKind kind() const override { return UpscalerKind::kFsr3; }

  bool fsr3_shared(Fsr3SharedResources* out) const override {
    if (!shared_[0] || !shared_[1] || !shared_[2]) return false;
    out->dilated_depth = &shared_[0];
    out->dilated_motion = &shared_[1];
    out->recon_prev_depth = &shared_[2];
    return true;
  }

 private:
  // FSR3 (unlike FSR2) expects the application to own three resources it
  // shares with later effects. They persist across frames and stay in
  // GENERAL: the backend transitions them as needed during the dispatch and
  // restores the declared state afterwards.
  bool CreateSharedResources() {
    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    if (ffxFsr3UpscalerGetSharedResourceDescriptions(&context_, &shared) != FFX_OK) {
      REC_ERROR("fsr3: shared resource descriptions query failed");
      return false;
    }
    const FfxCreateResourceDescription* descs[kSharedCount] = {
        &shared.dilatedDepth, &shared.dilatedMotionVectors,
        &shared.reconstructedPrevNearestDepth};
    for (u32 i = 0; i < kSharedCount; ++i) {
      const FfxResourceDescription& res = descs[i]->resourceDescription;
      Format format = ToFormat(res.format);
      if (format == Format::kUnknown) {
        REC_ERROR("fsr3: unexpected shared resource format ({})", static_cast<int>(res.format));
        return false;
      }
      // TRANSFER_DST because FSR clears these via vkCmdClearColorImage on
      // reset frames.
      TextureUsageFlags usage =
          kTextureUsageSampled | kTextureUsageStorage | kTextureUsageTransferDst;
      if (res.usage & FFX_RESOURCE_USAGE_RENDERTARGET) {
        usage |= kTextureUsageColorTarget;
      }
      shared_[i] = device_.CreateImage2D(format, {res.width, res.height}, usage);
      if (!shared_[i]) {
        REC_ERROR("fsr3: shared resource allocation failed");
        return false;
      }
      shared_descs_[i] = res;
    }

    // The dispatch declares them as UNORDERED_ACCESS (= GENERAL), so leave
    // them there from the start instead of UNDEFINED.
    device_.ImmediateSubmit([this](CommandList& cmd) {
      TextureBarrier barriers[kSharedCount];
      for (u32 i = 0; i < kSharedCount; ++i) {
        barriers[i] =
            Transition(shared_[i], ResourceState::kUndefined, ResourceState::kGeneral);
      }
      cmd.TextureBarriers({barriers, kSharedCount});
    });
    return true;
  }

  void Dispatch(PassContext& ctx, const UpscalerInputs& inputs, ResourceHandle output) {
    const GpuImage& color = ctx.graph->image(inputs.color);
    const GpuImage& depth = ctx.graph->image(inputs.depth);
    const GpuImage& motion = ctx.graph->image(inputs.motion_vectors);
    const GpuImage& out = ctx.graph->image(output);

    FfxFsr3UpscalerDispatchDescription dispatch{};
    dispatch.commandList = ffxGetCommandListVK(GetVkCommandBuffer(*ctx.cmd));
    dispatch.color =
        ffxGetResourceVK(GetVkImage(color), DescribeImage(color, FFX_RESOURCE_USAGE_READ_ONLY),
                         L"fsr3_color", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth =
        ffxGetResourceVK(GetVkImage(depth), DescribeImage(depth, FFX_RESOURCE_USAGE_READ_ONLY),
                         L"fsr3_depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors =
        ffxGetResourceVK(GetVkImage(motion), DescribeImage(motion, FFX_RESOURCE_USAGE_READ_ONLY),
                         L"fsr3_motion", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.dilatedDepth = ffxGetResourceVK(GetVkImage(shared_[0]), shared_descs_[0],
                                             L"fsr3_dilated_depth",
                                             FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.dilatedMotionVectors = ffxGetResourceVK(GetVkImage(shared_[1]), shared_descs_[1],
                                                     L"fsr3_dilated_motion",
                                                     FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.reconstructedPrevNearestDepth =
        ffxGetResourceVK(GetVkImage(shared_[2]), shared_descs_[2], L"fsr3_recon_depth",
                         FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.output = ffxGetResourceVK(GetVkImage(out), DescribeImage(out, FFX_RESOURCE_USAGE_UAV),
                                       L"fsr3_output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    // Same pixel-space values that were baked into the projection; the
    // motion target stores uv-space current->previous offsets, so scaling by
    // the render size yields the pixel-space vectors FSR expects.
    dispatch.jitterOffset = {inputs.jitter_x, inputs.jitter_y};
    dispatch.motionVectorScale = {static_cast<f32>(desc_.render_width),
                                  static_cast<f32>(desc_.render_height)};
    dispatch.renderSize = {desc_.render_width, desc_.render_height};
    dispatch.upscaleSize = {desc_.output_width, desc_.output_height};
    dispatch.enableSharpening = inputs.sharpness > 0.0f;
    dispatch.sharpness = inputs.sharpness;
    dispatch.frameTimeDelta = inputs.frame_delta_seconds * 1000.0f;  // fsr wants milliseconds
    dispatch.preExposure = 1.0f;
    dispatch.reset = inputs.reset_history;
    // Reversed-infinite depth: the header convention puts FLT_MAX in
    // cameraNear and the near plane distance in cameraFar.
    dispatch.cameraNear = FLT_MAX;
    dispatch.cameraFar = inputs.camera_near;
    dispatch.cameraFovAngleVertical = inputs.camera_fov_y;
    dispatch.viewSpaceToMetersFactor = 1.0f;

    FfxErrorCode err = ffxFsr3UpscalerContextDispatch(&context_, &dispatch);
    if (err != FFX_OK) {
      REC_ERROR("fsr3: dispatch failed ({})", static_cast<int>(err));
    }
  }

  void Destroy() {
    // The renderer waits for device idle before destroying upscalers.
    if (context_valid_) {
      ffxFsr3UpscalerContextDestroy(&context_);
      context_valid_ = false;
    }
    for (GpuImage& image : shared_) {
      if (image) device_.DestroyImage(image);
    }
    if (scratch_) {
      std::free(scratch_);
      scratch_ = nullptr;
    }
  }

  static constexpr u32 kSharedCount = 3;

  Device& device_;
  UpscalerDesc desc_;
  void* scratch_ = nullptr;
  size_t scratch_size_ = 0;
  FfxInterface interface_{};
  FfxFsr3UpscalerContext context_{};
  bool context_valid_ = false;
  GpuImage shared_[kSharedCount];  // dilated depth, dilated motion, recon prev depth
  FfxResourceDescription shared_descs_[kSharedCount]{};
};

}  // namespace

std::unique_ptr<Upscaler> CreateFsr3Upscaler(const UpscalerDesc& desc, Device& device) {
  auto upscaler = std::make_unique<Fsr3Upscaler>(device);
  if (!upscaler->Initialize(desc)) return nullptr;
  return upscaler;
}

}  // namespace rec::render

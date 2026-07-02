#include "render/post/frame_generation.h"

#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
// Vulkan escape hatch: the FFX backend speaks raw Vulkan. Also pulls volk
// (VK_NO_PROTOTYPES) before the ffx vk headers.
#include "render/rhi/vulkan_interop.h"

#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

namespace rec::render {
namespace {

Format ToFormat(FfxSurfaceFormat format) {
  switch (format) {
    case FFX_SURFACE_FORMAT_R32_FLOAT: return Format::kR32Float;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return Format::kRG16Float;
    case FFX_SURFACE_FORMAT_R16G16_SINT: return Format::kRG16Sint;
    case FFX_SURFACE_FORMAT_R32_UINT: return Format::kR32Uint;
    default: return Format::kUnknown;
  }
}

FfxResourceDescription DescribeImage(const GpuImage& image, FfxResourceUsage usage) {
  FfxResourceDescription desc{};
  desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  desc.format = ffxGetSurfaceFormatVK(GetVkFormat(image.format));
  desc.width = image.extent.width;
  desc.height = image.extent.height;
  desc.depth = 1;
  desc.mipCount = 1;
  desc.flags = FFX_RESOURCE_FLAGS_NONE;
  desc.usage = usage;
  return desc;
}

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

class FfxFrameGenerator final : public FrameGenerator {
 public:
  explicit FfxFrameGenerator(Device& device) : device_(device) {}
  ~FfxFrameGenerator() override { Destroy(); }

  bool Initialize(const FrameGenDesc& desc) {
    desc_ = desc;

    VulkanHandles h = GetVulkanHandles(device_);
    if (h.device == VK_NULL_HANDLE) {
      REC_WARN("framegen: requires the vulkan backend");
      return false;
    }

    VkDeviceContext device_context{h.device, h.physical_device, DeviceProcAddr};
    FfxDevice ffx_device = ffxGetDeviceVK(&device_context);
    constexpr u32 kContexts = 2;  // optical flow + frame interpolation
    scratch_size_ = ffxGetScratchMemorySizeVK(h.physical_device, kContexts);
    scratch_ = std::calloc(1, scratch_size_);
    if (!scratch_) return false;
    FfxErrorCode err =
        ffxGetInterfaceVK(&interface_, ffx_device, scratch_, scratch_size_, kContexts);
    if (err != FFX_OK) {
      REC_ERROR("framegen: backend interface creation failed ({})", static_cast<int>(err));
      return false;
    }

    // Optical flow runs on the presented (display-resolution) color.
    FfxOpticalflowContextDescription of_desc{};
    of_desc.backendInterface = interface_;
    of_desc.flags = 0;  // sdr backbuffer
    of_desc.resolution = {desc.display_width, desc.display_height};
    err = ffxOpticalflowContextCreate(&of_context_, &of_desc);
    if (err != FFX_OK) {
      REC_ERROR("framegen: optical flow context creation failed ({})", static_cast<int>(err));
      return false;
    }
    of_valid_ = true;

    // The engine renders reversed infinite depth; the backbuffer input is the
    // tonemapped sRGB-encoded UNORM image, so no HDR flag.
    FfxFrameInterpolationContextDescription fi_desc{};
    fi_desc.backendInterface = interface_;
    fi_desc.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED |
                    FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
    fi_desc.maxRenderSize = {desc.render_width, desc.render_height};
    fi_desc.displaySize = {desc.display_width, desc.display_height};
    // The interpolation source/output stay RGBA8 regardless of the swapchain's
    // channel order; the present path blits (which swizzles) into the real
    // backbuffer format.
    fi_desc.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fi_desc.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    err = ffxFrameInterpolationContextCreate(&fi_context_, &fi_desc);
    if (err != FFX_OK) {
      REC_ERROR("framegen: interpolation context creation failed ({})", static_cast<int>(err));
      return false;
    }
    fi_valid_ = true;

    if (!CreateSharedResources()) return false;

    REC_INFO("fsr3 frame generation ready: {}x{} interpolation ({}x{} guides)",
             desc.display_width, desc.display_height, desc.render_width, desc.render_height);
    return true;
  }

  bool Record(CommandList& cmd, const FrameGenInputs& in) override {
    FfxCommandList ffx_cmd = ffxGetCommandListVK(GetVkCommandBuffer(cmd));

    // 1) Optical flow over the pre-UI color (the ui would drag flow vectors).
    FfxOpticalflowDispatchDescription of{};
    of.commandList = ffx_cmd;
    of.color = Res(hudless_, FFX_RESOURCE_USAGE_READ_ONLY, L"fg_hudless",
                   FFX_RESOURCE_STATE_COMPUTE_READ);
    of.opticalFlowVector = Shared(kOfVector, L"fg_of_vector");
    of.opticalFlowSCD = Shared(kOfScd, L"fg_of_scd");
    of.reset = in.reset;
    of.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    of.minMaxLuminance = {0.0f, 1000.0f};
    if (ffxOpticalflowContextDispatch(&of_context_, &of) != FFX_OK) {
      REC_ERROR("framegen: optical flow dispatch failed");
      return false;
    }

    // 2) Interpolate between the previous and current backbuffer.
    FfxFrameInterpolationDispatchDescription fi{};
    fi.commandList = ffx_cmd;
    fi.displaySize = {desc_.display_width, desc_.display_height};
    fi.renderSize = {desc_.render_width, desc_.render_height};
    fi.currentBackBuffer = Res(*in.backbuffer, FFX_RESOURCE_USAGE_READ_ONLY, L"fg_backbuffer",
                               FFX_RESOURCE_STATE_COMPUTE_READ);
    // Interpolation sources the pre-UI copy; the renderer re-draws the UI on
    // the generated frame afterwards.
    fi.currentBackBuffer_HUDLess = Res(hudless_, FFX_RESOURCE_USAGE_READ_ONLY, L"fg_hudless",
                                       FFX_RESOURCE_STATE_COMPUTE_READ);
    fi.output = Res(interpolated_, FFX_RESOURCE_USAGE_UAV, L"fg_output",
                    FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    fi.interpolationRect = {0, 0, static_cast<i32>(desc_.display_width),
                            static_cast<i32>(desc_.display_height)};
    fi.opticalFlowVector = Shared(kOfVector, L"fg_of_vector");
    fi.opticalFlowSceneChangeDetection = Shared(kOfScd, L"fg_of_scd");
    fi.opticalFlowBufferSize = {shared_[kOfVector].extent.width,
                                shared_[kOfVector].extent.height};
    fi.opticalFlowScale = {1.0f / static_cast<f32>(desc_.display_width),
                           1.0f / static_cast<f32>(desc_.display_height)};
    fi.opticalFlowBlockSize = 8;
    fi.cameraNear = FLT_MAX;
    fi.cameraFar = in.camera_near;
    fi.cameraFovAngleVertical = in.camera_fov_y;
    fi.viewSpaceToMetersFactor = 1.0f;
    fi.frameTimeDelta = in.frame_delta_seconds * 1000.0f;
    fi.reset = in.reset;
    fi.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fi.minMaxLuminance[0] = 0.0f;
    fi.minMaxLuminance[1] = 1000.0f;
    fi.frameID = in.frame_id;
    // The upscaler's shared resources, declared exactly as it declares them.
    fi.dilatedDepth = Res(*in.dilated_depth, FFX_RESOURCE_USAGE_UAV, L"fg_dilated_depth",
                          FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    fi.dilatedMotionVectors = Res(*in.dilated_motion, FFX_RESOURCE_USAGE_UAV,
                                  L"fg_dilated_motion", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    fi.reconstructedPrevDepth = Res(*in.recon_prev_depth, FFX_RESOURCE_USAGE_UAV,
                                    L"fg_recon_depth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    if (ffxFrameInterpolationDispatch(&fi_context_, &fi) != FFX_OK) {
      REC_ERROR("framegen: interpolation dispatch failed");
      return false;
    }
    return true;
  }

  const GpuImage& interpolated() const override { return interpolated_; }
  const GpuImage& hudless() const override { return hudless_; }

 private:
  // Shared resource slots (app-owned, persistent, kept in GENERAL).
  enum SharedSlot : u32 {
    kOfVector = 0,
    kOfScd,
    kSharedCount,
  };

  bool CreateSharedResources() {
    FfxOpticalflowSharedResourceDescriptions of_shared{};
    if (ffxOpticalflowGetSharedResourceDescriptions(&of_context_, &of_shared) != FFX_OK) {
      REC_ERROR("framegen: optical flow shared resource query failed");
      return false;
    }
    const FfxCreateResourceDescription* descs[kSharedCount] = {
        &of_shared.opticalFlowVector, &of_shared.opticalFlowSCD};
    for (u32 i = 0; i < kSharedCount; ++i) {
      const FfxResourceDescription& res = descs[i]->resourceDescription;
      Format format = ToFormat(res.format);
      if (format == Format::kUnknown) {
        REC_ERROR("framegen: unexpected shared resource format ({})",
                  static_cast<int>(res.format));
        return false;
      }
      shared_[i] = device_.CreateImage2D(
          format, {res.width, res.height},
          kTextureUsageSampled | kTextureUsageStorage | kTextureUsageTransferDst);
      if (!shared_[i]) {
        REC_ERROR("framegen: shared resource allocation failed");
        return false;
      }
      shared_descs_[i] = res;
    }

    // The interpolated output: UAV-written by FI, transfer-blitted into the
    // swapchain image (the blit also converts to the surface's channel order).
    interpolated_ = device_.CreateImage2D(
        Format::kRGBA8Unorm, {desc_.display_width, desc_.display_height},
        kTextureUsageStorage | kTextureUsageSampled | kTextureUsageTransferSrc);
    if (!interpolated_) {
      REC_ERROR("framegen: interpolated target allocation failed");
      return false;
    }
    // Pre-UI interpolation source, blitted from the backbuffer each frame.
    // TRANSFER_SRC as well: the FI context copies it into its internal
    // previous-interpolation-source buffer each dispatch.
    hudless_ = device_.CreateImage2D(
        Format::kRGBA8Unorm, {desc_.display_width, desc_.display_height},
        kTextureUsageSampled | kTextureUsageTransferDst | kTextureUsageTransferSrc);
    if (!hudless_) {
      REC_ERROR("framegen: hudless target allocation failed");
      return false;
    }

    device_.ImmediateSubmit([this](CommandList& cmd) {
      TextureBarrier barriers[kSharedCount + 1];
      for (u32 i = 0; i < kSharedCount; ++i) {
        barriers[i] = Transition(shared_[i], ResourceState::kUndefined, ResourceState::kGeneral);
      }
      barriers[kSharedCount] =
          Transition(interpolated_, ResourceState::kUndefined, ResourceState::kGeneral);
      cmd.TextureBarriers({barriers, kSharedCount + 1});
      TextureBarrier hudless_init =
          Transition(hudless_, ResourceState::kUndefined, ResourceState::kShaderReadCompute);
      cmd.TextureBarriers({&hudless_init, 1});
    });
    return true;
  }

  FfxResource Res(const GpuImage& image, FfxResourceUsage usage, const wchar_t* name,
                  FfxResourceStates state) {
    return ffxGetResourceVK(GetVkImage(image), DescribeImage(image, usage), name, state);
  }

  FfxResource Shared(SharedSlot slot, const wchar_t* name) {
    return ffxGetResourceVK(GetVkImage(shared_[slot]), shared_descs_[slot], name,
                            FFX_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  void Destroy() {
    // The renderer waits for device idle before destroying the generator.
    if (fi_valid_) {
      ffxFrameInterpolationContextDestroy(&fi_context_);
      fi_valid_ = false;
    }
    if (of_valid_) {
      ffxOpticalflowContextDestroy(&of_context_);
      of_valid_ = false;
    }
    for (GpuImage& image : shared_) {
      if (image) device_.DestroyImage(image);
    }
    if (interpolated_) device_.DestroyImage(interpolated_);
    if (hudless_) device_.DestroyImage(hudless_);
    if (scratch_) {
      std::free(scratch_);
      scratch_ = nullptr;
    }
  }

  Device& device_;
  FrameGenDesc desc_;
  void* scratch_ = nullptr;
  size_t scratch_size_ = 0;
  FfxInterface interface_{};
  FfxOpticalflowContext of_context_{};
  FfxFrameInterpolationContext fi_context_{};
  bool of_valid_ = false;
  bool fi_valid_ = false;
  GpuImage shared_[kSharedCount];
  FfxResourceDescription shared_descs_[kSharedCount]{};
  GpuImage interpolated_;
  GpuImage hudless_;
};

}  // namespace

std::unique_ptr<FrameGenerator> CreateFrameGenerator(Device& device, const FrameGenDesc& desc) {
  auto generator = std::make_unique<FfxFrameGenerator>(device);
  if (!generator->Initialize(desc)) return nullptr;
  return generator;
}

}  // namespace rec::render

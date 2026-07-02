#include "render/gi/denoiser_rr.h"

#include "core/log.h"
#include "render/post/ngx_context.h"
#include "render/rhi/device.h"
// Vulkan escape hatch: NGX speaks raw Vulkan. Also pulls volk
// (VK_NO_PROTOTYPES) before the ngx vk headers.
#include "render/rhi/vulkan_interop.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

namespace rec::render {

bool RrDenoiser::Initialize(Device& device, Extent2D extent) {
  if (!ngx::Acquire(device)) return false;
  ngx_acquired_ = true;

  // Availability probe. Like the upscaler, treat "needs newer driver" as the
  // only hard stop: the availability flag stays 0 on self-contained setups
  // where NGX's updater/DRS probe cannot run, and feature creation below is
  // the authoritative test (it fails cleanly when the dlssd snippet is
  // missing, e.g. no aarch64 build shipped yet).
  int needs_driver = 0;
  NVSDK_NGX_Parameter_GetI(ngx::Capability(),
                           NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                           &needs_driver);
  if (needs_driver) {
    REC_WARN("dlss-rr: driver too old for ray reconstruction");
    Destroy(device);
    return false;
  }

  if (NVSDK_NGX_VULKAN_AllocateParameters(&params_) != NVSDK_NGX_Result_Success || !params_) {
    REC_WARN("dlss-rr: parameter allocation failed");
    Destroy(device);
    return false;
  }

  if (!CreateFeature(device, extent)) {
    Destroy(device);
    return false;
  }
  REC_INFO("dlss ray reconstruction ready ({}x{}, native)", extent.width, extent.height);
  return true;
}

bool RrDenoiser::CreateFeature(Device& device, Extent2D extent) {
  extent_ = extent;
  VulkanHandles h = GetVulkanHandles(device);
  bool created = false;
  device.ImmediateSubmit([&](CommandList& cmd) {
    NVSDK_NGX_DLSSD_Create_Params create{};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;  // normals.w
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;             // reversed-inf-z
    create.InWidth = extent.width;
    create.InHeight = extent.height;
    create.InTargetWidth = extent.width;  // native: denoise-only, no upscale
    create.InTargetHeight = extent.height;
    create.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    // Linear HDR radiance, reversed depth. Motion vectors are full render
    // resolution (no MVLowRes: render == target here).
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSSD_EXT1(h.device, GetVkCommandBuffer(cmd), 1, 1,
                                                      &handle_, params_, &create);
    created = r == NVSDK_NGX_Result_Success;
    if (!created) {
      REC_WARN("dlss-rr: feature creation failed ({:#x}) - dlssd snippet missing?",
               static_cast<u32>(r));
    }
  });
  return created;
}

void RrDenoiser::ReleaseFeature() {
  if (handle_) {
    NVSDK_NGX_VULKAN_ReleaseFeature(handle_);
    handle_ = nullptr;
  }
}

void RrDenoiser::Resize(Device& device, Extent2D extent) {
  if (!available()) return;
  if (extent == extent_) return;
  device.WaitIdle();
  ReleaseFeature();
  if (!CreateFeature(device, extent)) {
    REC_WARN("dlss-rr: resize recreation failed, denoiser disabled");
    Destroy(device);
  }
}

void RrDenoiser::Destroy(Device& device) {
  (void)device;  // the renderer waits for idle before teardown
  ReleaseFeature();
  if (params_) {
    NVSDK_NGX_VULKAN_DestroyParameters(params_);
    params_ = nullptr;
  }
  if (ngx_acquired_) {
    ngx::Release();
    ngx_acquired_ = false;
  }
}

void RrDenoiser::AddToGraph(RenderGraph& graph, const Inputs& inputs, ResourceHandle output,
                            const Frame& frame) {
  graph.AddPass(
      "dlss_rr",
      [&](RenderGraph::PassBuilder& builder) {
        for (ResourceHandle h : {inputs.color, inputs.depth, inputs.motion, inputs.normals_rough,
                                 inputs.diffuse_albedo, inputs.specular_albedo}) {
          builder.Read(h, ResourceUsage::kSampledCompute);
        }
        builder.Write(output, ResourceUsage::kStorageWrite);
      },
      [this, inputs, output, frame](PassContext& ctx) {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        auto wrap = [&](ResourceHandle handle, bool read_write) {
          const GpuImage& image = ctx.graph->image(handle);
          return NVSDK_NGX_Create_ImageView_Resource_VK(
              GetVkImageView(image.view), GetVkImage(image), range, GetVkFormat(image.format),
              image.extent.width, image.extent.height, read_write);
        };
        NVSDK_NGX_Resource_VK color = wrap(inputs.color, false);
        NVSDK_NGX_Resource_VK depth = wrap(inputs.depth, false);
        NVSDK_NGX_Resource_VK motion = wrap(inputs.motion, false);
        NVSDK_NGX_Resource_VK normals = wrap(inputs.normals_rough, false);
        NVSDK_NGX_Resource_VK diffuse_albedo = wrap(inputs.diffuse_albedo, false);
        NVSDK_NGX_Resource_VK specular_albedo = wrap(inputs.specular_albedo, false);
        NVSDK_NGX_Resource_VK out = wrap(output, true);

        // Column-major Mat4 passed as float*; NGX consumes the same layout.
        Mat4 world_to_view = frame.world_to_view;
        Mat4 view_to_clip = frame.view_to_clip;

        NVSDK_NGX_VK_DLSSD_Eval_Params eval{};
        eval.pInColor = &color;
        eval.pInOutput = &out;
        eval.pInDepth = &depth;
        eval.pInMotionVectors = &motion;
        eval.pInNormals = &normals;  // roughness packed in .w (create params)
        eval.pInDiffuseAlbedo = &diffuse_albedo;
        eval.pInSpecularAlbedo = &specular_albedo;
        eval.InJitterOffsetX = 0.0f;  // the recon path traces unjittered rays
        eval.InJitterOffsetY = 0.0f;
        eval.InRenderSubrectDimensions = {extent_.width, extent_.height};
        eval.InReset = frame.reset ? 1 : 0;
        // The motion target stores uv-space current->previous offsets; scaling
        // by the render size yields the pixel-space vectors DLSS expects.
        eval.InMVScaleX = static_cast<f32>(extent_.width);
        eval.InMVScaleY = static_cast<f32>(extent_.height);
        eval.pInWorldToViewMatrix = world_to_view.m;
        eval.pInViewToClipMatrix = view_to_clip.m;
        eval.InFrameTimeDeltaInMsec = frame.frame_delta_ms;

        NVSDK_NGX_Result r =
            NGX_VULKAN_EVALUATE_DLSSD_EXT(GetVkCommandBuffer(*ctx.cmd), handle_, params_, &eval);
        if (r != NVSDK_NGX_Result_Success) {
          REC_ERROR("dlss-rr: evaluate failed ({:#x})", static_cast<u32>(r));
        }
      });
}

}  // namespace rec::render

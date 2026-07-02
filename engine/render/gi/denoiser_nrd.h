#ifndef RECREATION_RENDER_DENOISER_NRD_H_
#define RECREATION_RENDER_DENOISER_NRD_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"
// Vulkan-only module (compiled under RECREATION_HAS_NRD, which implies the
// Vulkan backend): the internals speak raw Vulkan through the interop escape
// hatch, the public surface below stays rhi-typed.
#include "render/rhi/vulkan_interop.h"

namespace nrd {
struct Instance;
struct DispatchDesc;
}  // namespace nrd

namespace rec::render {

class Device;

// NVIDIA NRD real time denoiser, driven through its direct compute path: NRD
// hands back a list of compute dispatches each frame and this class records
// them into the engine's command buffer. One instance hosts both the RTAO
// (REBLUR_DIFFUSE_OCCLUSION) and the sun shadow (SIGMA_SHADOW) denoisers; they
// share NRD's internal texture pools and the per-frame camera state.
//
// The noisy inputs are produced by the engine (packed in NRD's own encoding,
// see the *_pack/*_trace shaders) and the denoised outputs are owned here and
// imported into the render graph so later passes can sample them.
class NrdDenoiser {
 public:
  bool Initialize(Device& device, Extent2D extent);
  void Resize(Device& device, Extent2D extent);
  void Destroy(Device& device);

  bool available() const { return instance_ != nullptr; }

  // Per-frame camera state, set once before the Denoise* calls. Matrices are
  // column-major, non-jittered. Jitter is in pixels (converted to NRD's uv
  // convention internally).
  struct FrameSettings {
    Mat4 view_to_clip;
    Mat4 view_to_clip_prev;
    Mat4 world_to_view;
    Mat4 world_to_view_prev;
    f32 jitter[2] = {0, 0};
    f32 jitter_prev[2] = {0, 0};
    Vec3 sun_direction{0, -1, 0};  // travel direction of the light
    u32 frame_index = 0;
    // Path-tracer diffuse history length (frames). Lower = more responsive (less
    // ghosting/lag) but noisier; the ao/shadow denoisers ignore this.
    u32 diffuse_accumulated_frames = 16;
    bool reset = false;
  };
  void SetFrame(const FrameSettings& settings);

  // Builds NRD's shared guide inputs (IN_NORMAL_ROUGHNESS, IN_VIEWZ) from the
  // engine g-buffer. Returns transient graph handles.
  struct Inputs {
    ResourceHandle normal_roughness = kInvalidResource;
    ResourceHandle view_z = kInvalidResource;
  };
  Inputs PrepareInputs(RenderGraph& graph, ResourceHandle depth, ResourceHandle normals,
                       f32 near_plane);

  // REBLUR ambient occlusion. in_hitdist holds the packed normalized AO hit
  // distance (R8). Returns the denoised AO handle (R8 normalized hit distance).
  ResourceHandle DenoiseAo(RenderGraph& graph, ResourceHandle normal_roughness,
                           ResourceHandle view_z, ResourceHandle motion,
                           ResourceHandle in_hitdist);

  // SIGMA sun shadow. in_penumbra holds the packed penumbra (R16f). Returns the
  // denoised shadow handle (R8, decode with shadow*shadow).
  ResourceHandle DenoiseShadow(RenderGraph& graph, ResourceHandle normal_roughness,
                               ResourceHandle view_z, ResourceHandle motion,
                               ResourceHandle in_penumbra);

  // REBLUR diffuse radiance, for the path tracer. in_radiance_hitdist holds the
  // (albedo-demodulated) radiance + normalized hit distance, packed by
  // REBLUR_FrontEnd_PackRadianceAndNormHitDist (RGBA16f). Returns the denoised
  // radiance handle; unpack with REBLUR_BackEnd_UnpackRadianceAndNormHitDist.
  // REBLUR specular radiance, for the hybrid reflections. Same packing as
  // the diffuse variant; xyz of the output is the denoised radiance.
  ResourceHandle DenoiseSpecular(RenderGraph& graph, ResourceHandle normal_roughness,
                                 ResourceHandle view_z, ResourceHandle motion,
                                 ResourceHandle in_radiance_hitdist);

  ResourceHandle DenoiseDiffuse(RenderGraph& graph, ResourceHandle normal_roughness,
                                ResourceHandle view_z, ResourceHandle motion,
                                ResourceHandle in_radiance_hitdist);

  // NRD encodings the engine shaders must match.
  static constexpr Format kNormalRoughnessFormat = Format::kRGB10A2Unorm;
  static constexpr Format kViewZFormat = Format::kR16Float;
  static constexpr Format kHitDistFormat = Format::kR8Unorm;
  static constexpr Format kPenumbraFormat = Format::kR16Float;
  static constexpr Format kShadowFormat = Format::kR8Unorm;
  // Diffuse radiance + normalized hit distance, in and out (RGBA16f).
  static constexpr Format kDiffuseRadianceFormat = Format::kRGBA16Float;
  // REBLUR hit distance normalization params (A, B, C), shared with the shader.
  static constexpr f32 kHitDistParams[3] = {3.0f, 0.1f, 20.0f};

 private:
  struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout resource_set_layout = VK_NULL_HANDLE;
    u32 texture_num = 0;  // SRVs in the resources set
    u32 storage_num = 0;  // UAVs in the resources set
    bool has_constants = false;
  };

  struct PoolTexture {
    GpuImage image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };

  // Per-frame map from NRD ResourceType to a concrete view/image, filled before
  // recording a denoiser's dispatches.
  struct ResourceBinding {
    VkImageView view = VK_NULL_HANDLE;
    PoolTexture* tracked = nullptr;  // non-null for owned (pool/output) textures
  };

  bool CreatePipelines(Device& device);
  bool CreatePackPipeline(Device& device);
  bool CreateDescriptorPools();
  void CreatePools(Device& device, Extent2D extent);
  void DestroyPools(Device& device);
  // Per-dispatch descriptor sets, from the frame-parity pool reset in SetFrame.
  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);
  // noisy_type / output_type carry an nrd::ResourceType value as an int so this
  // header stays free of NRD types (the renderer includes it transitively).
  ResourceHandle AddDenoisePass(RenderGraph& graph, u32 identifier, const char* pass_name,
                                ResourceHandle normal_roughness, ResourceHandle view_z,
                                ResourceHandle motion, ResourceHandle noisy, int noisy_type,
                                PoolTexture& output, int output_type, const char* output_name,
                                ResourceState* output_state);
  void RecordDispatches(PassContext& ctx, u32 identifier);

  Device* device_ = nullptr;
  VkDevice vk_device_ = VK_NULL_HANDLE;  // raw handle via GetVulkanHandles
  nrd::Instance* instance_ = nullptr;
  Extent2D extent_{};

  base::Vector<Pipeline> pipelines_;
  base::Vector<PoolTexture> permanent_;
  base::Vector<PoolTexture> transient_;
  PoolTexture out_ao_;
  PoolTexture out_shadow_;
  PoolTexture out_diffuse_;
  PoolTexture out_specular_;
  // Render-graph import states for the denoised outputs (the graph reads the
  // starting state and writes the post-pass state back each frame).
  ResourceState out_ao_state_ = ResourceState::kUndefined;
  ResourceState out_shadow_state_ = ResourceState::kUndefined;
  ResourceState out_diffuse_state_ = ResourceState::kUndefined;
  ResourceState out_specular_state_ = ResourceState::kUndefined;

  // Engine-side input packing (g-buffer -> NRD guides), not part of NRD itself.
  VkPipeline pack_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout pack_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout pack_set_layout_ = VK_NULL_HANDLE;

  VkSampler samplers_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDescriptorSetLayout const_set_layout_ = VK_NULL_HANDLE;  // constant buffer + samplers
  // Internal per-frame descriptor pools, double buffered by frame parity like
  // the constant ring; the active parity's pool is reset in SetFrame.
  VkDescriptorPool descriptor_pools_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  u32 pool_parity_ = 0;
  GpuBuffer constant_ring_;  // host visible, dynamic-offset uniform buffer
  u64 constant_slot_size_ = 0;
  u32 constant_slot_count_ = 0;
  u32 constant_cursor_ = 0;

  // Register spaces / offsets read from the instance, applied to SPIR-V bindings.
  u32 resources_space_ = 0;
  u32 const_samplers_space_ = 0;
  u32 constant_register_ = 0;
  u32 sampler_base_register_ = 0;
  u32 resource_base_register_ = 0;
  u32 sampler_num_ = 0;

  // Active per-denoiser resolution table (ResourceType -> binding), valid only
  // while recording a single Denoise pass.
  ResourceBinding bindings_[64]{};
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_DENOISER_NRD_H_

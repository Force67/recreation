#ifndef RECREATION_RENDER_MESH_PIPELINE_H_
#define RECREATION_RENDER_MESH_PIPELINE_H_

#include <memory>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

// Per frame camera and lighting state, bound as set 0. Layout matches the
// std140 block in mesh.vert/mesh.frag.
struct FrameGlobals {
  Mat4 view_proj;
  Mat4 prev_view_proj;
  Mat4 inv_view_proj;
  f32 jitter[2] = {0, 0};  // ndc units
  f32 prev_jitter[2] = {0, 0};
  f32 sun_direction[4] = {0, -1, 0, 4};  // xyz travel direction, w intensity
  f32 sun_color[4] = {1, 1, 1, 0.06f};   // rgb color, w flat ambient when ibl off
  f32 camera_position[4] = {0, 0, 0, 1};  // xyz eye, w ibl intensity
  f32 misc[4] = {0, 0, 0, 0};  // x,y render size, z sun angular radius, w frame index
  u32 flags = 0;
  f32 time = 0;  // seconds, drives water waves
  u32 debug_view = 0;  // render::DebugView, isolates a shading channel
  f32 reflection_cutoff = 0.6f;  // roughness above which rt reflections fall back to ibl
};

// FrameGlobals::flags bits, mirrored in mesh.ps.hlsl.
inline constexpr u32 kFrameFlagIbl = 1u << 0;
inline constexpr u32 kFrameFlagAoValid = 1u << 1;
inline constexpr u32 kFrameFlagDdgi = 1u << 2;
inline constexpr u32 kFrameFlagWaterRt = 1u << 3;
inline constexpr u32 kFrameFlagReflections = 1u << 4;  // opaque rt specular reflections
inline constexpr u32 kFrameFlagRtShadows = 1u << 5;    // trace sun shadow in the rt variant
inline constexpr u32 kFrameFlagShadowMap = 1u << 6;    // sample cascaded shadow maps (raster path)

// model + prev_model are 128 bytes; skinned draws append the bone palette's
// buffer device address and this mesh's offset into it (needs a 144 byte push
// range, available on every desktop GPU). Non-skinned shaders ignore the tail.
struct MeshPushConstants {
  Mat4 model;
  Mat4 prev_model;
  u64 bone_address = 0;  // VkDeviceAddress of the frame bone palette, 0 = none
  u32 skin_offset = 0;   // first bone of this mesh in the palette
  u32 pad = 0;
};

// Forward pbr pipeline: classic vertex buffer, metallic roughness shading,
// reversed z depth. Outputs hdr color and motion vectors. Set 0 is the frame
// globals (plus the TLAS when raytracing is available), set 1 the material.
// Variants cover ray queried shadows on/off and a wireframe debug fill mode,
// all sharing one layout so they swap mid-frame without rebinding sets.
class MeshPipeline {
 public:
  // bindless_layout enables set 3 (the scene tables the rt variant reads for
  // reflection hit shading); pass VK_NULL_HANDLE when ray query is unavailable.
  static std::unique_ptr<MeshPipeline> Create(Device& device, VkFormat color_format,
                                              VkFormat motion_format, VkFormat normal_format,
                                              VkFormat depth_format,
                                              VkDescriptorSetLayout material_layout,
                                              VkDescriptorSetLayout environment_layout,
                                              VkDescriptorSetLayout bindless_layout);
  ~MeshPipeline();

  MeshPipeline(const MeshPipeline&) = delete;
  MeshPipeline& operator=(const MeshPipeline&) = delete;

  VkDescriptorSetLayout set_layout() const { return set_layout_; }
  bool has_rt_variant() const { return pipelines_[kRt] != VK_NULL_HANDLE; }

  // use_rt selects the ray-query fragment variant (shadows and/or reflections);
  // bindless is bound as set 3 when the pipeline was built with it.
  void Bind(VkCommandBuffer cmd, VkDescriptorSet globals, VkDescriptorSet environment,
            VkDescriptorSet bindless, bool use_rt, bool wireframe);
  void BindPrepass(VkCommandBuffer cmd, VkDescriptorSet globals);
  // Transparent variant: alpha blend over the opaque result, depth tested
  // against the prepass without writing. Set state mirrors Bind.
  void BindBlend(VkCommandBuffer cmd, VkDescriptorSet globals, VkDescriptorSet environment,
                 VkDescriptorSet bindless, bool use_rt);
  void BindMaterial(VkCommandBuffer cmd, VkDescriptorSet material);
  void Draw(VkCommandBuffer cmd, const GpuMesh& mesh, const MeshPushConstants& push);
  void DrawSubmesh(VkCommandBuffer cmd, const GpuSubmesh& submesh);

  // Swap the bound pipeline between the static and GPU-skinned vertex paths
  // mid-pass without rebinding descriptor sets (the layout is shared). The draw
  // loop calls these when it crosses a skinned/non-skinned mesh boundary.
  bool has_skinning() const { return skinned_pipelines_[0] != VK_NULL_HANDLE; }
  void SetSkinned(VkCommandBuffer cmd, bool skinned, bool use_rt, bool wireframe);
  void SetPrepassSkinned(VkCommandBuffer cmd, bool skinned);

 private:
  // Variant index bits.
  static constexpr u32 kRt = 1;
  static constexpr u32 kWire = 2;

  explicit MeshPipeline(Device& device) : device_(device) {}

  Device& device_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  bool has_bindless_ = false;  // set 3 present in the layout
  VkPipeline pipelines_[4] = {};  // [rt | wire]
  VkPipeline blend_pipelines_[2] = {};  // [rt]
  VkPipeline prepass_pipeline_ = VK_NULL_HANDLE;
  // Skinned vertex path: same fragment variants, extra vertex stream + VS.
  VkPipeline skinned_pipelines_[2] = {};  // [rt]
  VkPipeline skinned_prepass_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_MESH_PIPELINE_H_

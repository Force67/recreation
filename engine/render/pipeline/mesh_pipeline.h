#ifndef RECREATION_RENDER_MESH_PIPELINE_H_
#define RECREATION_RENDER_MESH_PIPELINE_H_

#include <memory>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

// A dynamic omni light, accumulated in the forward lighting pass. Packed in
// float4s so the StructuredBuffer stride matches the shader exactly.
// Dynamic local light. The first two rows are the legacy point-light layout;
// direction/params extend it to spots and representative-point area lights.
// type: 0 point, 1 spot, 2 sphere area, 3 rect area.
struct PointLight {
  f32 pos_radius[4] = {0, 0, 0, 1};       // xyz position, w influence radius (meters)
  f32 color_intensity[4] = {1, 1, 1, 1};  // rgb color, w intensity
  f32 direction_type[4] = {0, -1, 0, 0};  // xyz emit direction, w type
  // spot: x cos(inner), y cos(outer). sphere area: x source radius (m).
  // rect area: x,y half extents (m); the basis derives from the direction.
  f32 params[4] = {0.9f, 0.7f, 0, 0};
};

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
  u32 ao_ray_count = 0;  // rt ao rays/pixel this frame (0 when ao is screen-space), for the ray-count view
  u32 light_count = 0;   // dynamic point lights in the bound light buffer
  f32 pad_wind[2] = {0, 0};  // aligns wind to the hlsl float4 register
  // xyz wind direction * strength (m of sway at weight 1), w gust frequency.
  f32 wind[4] = {0.6f, 0.0f, 0.35f, 1.0f};
  // Froxel light clustering: x slice scale, y slice bias (exponential view-z
  // slicing), z,w tile size in pixels.
  f32 cluster_params[4] = {0, 0, 64, 64};
};

// A projected decal: an oriented box whose -z face carries an atlas region,
// blended into the surface albedo before shading (clustered like the lights).
struct Decal {
  // World -> unit-box rows (decal space: xyz in [-1,1], z along the normal).
  f32 row0[4] = {1, 0, 0, 0};
  f32 row1[4] = {0, 1, 0, 0};
  f32 row2[4] = {0, 0, 1, 0};
  f32 uv_rect[4] = {1, 1, 0, 0};     // atlas uv scale.xy, offset.zw
  f32 tint_blend[4] = {1, 1, 1, 1};  // rgb tint, w albedo blend strength
  // x: normal-map strength (0 = albedo only), y: roughness multiplier the
  // decal's alpha blends toward (1 = unchanged), z: emissive strength
  // (albedo * z adds to the surface emission), w: unused.
  f32 params2[4] = {0, 1, 0, 0};
};

// Froxel grid for clustered lighting/decals (mirrored in the shaders).
inline constexpr u32 kClusterTilesX = 16;
inline constexpr u32 kClusterTilesY = 9;
inline constexpr u32 kClusterSlices = 24;
inline constexpr u32 kClusterCount = kClusterTilesX * kClusterTilesY * kClusterSlices;
inline constexpr u32 kMaxLightsPerCluster = 32;
inline constexpr u32 kMaxDecalsPerCluster = 16;
inline constexpr u32 kMaxFrameDecals = 128;

// FrameGlobals::flags bits, mirrored in mesh.ps.hlsl.
inline constexpr u32 kFrameFlagIbl = 1u << 0;
inline constexpr u32 kFrameFlagAoValid = 1u << 1;
inline constexpr u32 kFrameFlagDdgi = 1u << 2;
inline constexpr u32 kFrameFlagWaterRt = 1u << 3;
inline constexpr u32 kFrameFlagReflections = 1u << 4;  // opaque rt specular reflections
inline constexpr u32 kFrameFlagRtShadows = 1u << 5;    // trace sun shadow in the rt variant
inline constexpr u32 kFrameFlagShadowMap = 1u << 6;    // sample cascaded shadow maps (raster path)
inline constexpr u32 kFrameFlagSigmaShadow = 1u << 7;  // sample the SIGMA-denoised sun shadow
inline constexpr u32 kFrameFlagAurora = 1u << 8;       // draw the night-sky aurora (sky.ps)
inline constexpr u32 kFrameFlagSpecReflTex = 1u << 9;  // sample the denoised reflection target
inline constexpr u32 kFrameFlagRestirDi = 1u << 10;     // point/spot lights come from the ReSTIR DI textures

// model + prev_model are 128 bytes; skinned draws append the bone palette's
// buffer device address and this mesh's offset into it (needs a 144 byte push
// range, available on every desktop GPU). Non-skinned shaders ignore the tail.
struct MeshPushConstants {
  Mat4 model;
  Mat4 prev_model;
  u64 bone_address = 0;  // device address of the frame bone palette, 0 = none
  u32 skin_offset = 0;   // first bone of this mesh in the palette
  u32 tint_packed = 0;   // per-draw rgb8 tint (0xRRGGBB) modulating albedo, 0 = none
};

// Push constants for the optional mesh-shader opaque path. The geometry buffers
// are read in the mesh shader by device address; layout matches mesh_scene.ms.
struct MeshShaderPush {
  Mat4 model;
  Mat4 prev_model;
  f32 bounds[4] = {0, 0, 0, 0};  // xyz model-space center, w radius (task-stage cull)
  f32 occlusion[4] = {0, 0, 0, 0};  // proj.m00, proj.m11, hiz w, hiz h (w==0 disables)
  u64 meshlets_address = 0;
  u64 meshlet_vertices_address = 0;
  u64 meshlet_triangles_address = 0;
  u64 vertices_address = 0;
  u32 meshlet_offset = 0;
  u32 meshlet_count = 0;
};

// Forward pbr pipeline: classic vertex buffer, metallic roughness shading,
// reversed z depth. Outputs hdr color and motion vectors. Set 0 is the frame
// globals (plus the TLAS when raytracing is available), set 1 the material.
// Variants cover ray queried shadows on/off and a wireframe debug fill mode,
// all sharing one layout so they swap mid-frame without rebinding sets.
class MeshPipeline {
 public:
  // Third scene-pass attachment: diffuse-only lighting of skin materials plus
  // a mask, consumed by the screen-space subsurface scattering blur.
  static constexpr Format kSkinDiffuseFormat = Format::kRGBA16Float;

  // bindless_layout enables set 3 (the scene tables the rt variant reads for
  // reflection hit shading); pass a null handle when ray query is unavailable.
  static std::unique_ptr<MeshPipeline> Create(Device& device, Format color_format,
                                              Format motion_format, Format normal_format,
                                              Format depth_format,
                                              BindingLayoutHandle material_layout,
                                              BindingLayoutHandle environment_layout,
                                              BindingLayoutHandle bindless_layout);
  ~MeshPipeline();

  MeshPipeline(const MeshPipeline&) = delete;
  MeshPipeline& operator=(const MeshPipeline&) = delete;

  BindingLayoutHandle set_layout() const { return set_layout_; }
  bool has_rt_variant() const { return static_cast<bool>(pipelines_[kRt]); }

  // use_rt selects the ray-query fragment variant (shadows and/or reflections);
  // bindless is bound as set 3 when the pipeline was built with it.
  void Bind(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment,
            BindingSetHandle bindless, bool use_rt, bool wireframe);
  void BindPrepass(CommandList& cmd, BindingSetHandle globals);
  // Transparent variant: alpha blend over the opaque result, depth tested
  // against the prepass without writing. Set state mirrors Bind.
  void BindBlend(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment,
                 BindingSetHandle bindless, bool use_rt);
  void BindMaterial(CommandList& cmd, BindingSetHandle material);
  void Draw(CommandList& cmd, const GpuMesh& mesh, const MeshPushConstants& push);
  void DrawSubmesh(CommandList& cmd, const GpuSubmesh& submesh);

  // Optional mesh-shader opaque path. Built only when the device supports it;
  // the scene/prepass variants reuse the same descriptor sets and fragment
  // shaders as the raster path. BindMeshMaterial binds set 1 with the
  // mesh-shader layout; DrawMeshlets issues one workgroup per meshlet.
  bool has_mesh_shader() const {
    return static_cast<bool>(ms_scene_[0]) && static_cast<bool>(ms_prepass_);
  }
  void BindMeshScene(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment,
                     BindingSetHandle bindless, bool use_rt);
  void BindMeshPrepass(CommandList& cmd, BindingSetHandle globals);
  void BindMeshMaterial(CommandList& cmd, BindingSetHandle material);
  void DrawMeshlets(CommandList& cmd, const MeshShaderPush& push);

  // Swap the bound pipeline between the static and GPU-skinned vertex paths
  // mid-pass without rebinding descriptor sets (the layout is shared). The draw
  // loop calls these when it crosses a skinned/non-skinned mesh boundary.
  bool has_skinning() const { return static_cast<bool>(skinned_pipelines_[0]); }
  void SetSkinned(CommandList& cmd, bool skinned, bool use_rt, bool wireframe);
  void SetPrepassSkinned(CommandList& cmd, bool skinned);

 private:
  // Variant index bits.
  static constexpr u32 kRt = 1;
  static constexpr u32 kWire = 2;

  explicit MeshPipeline(Device& device) : device_(device) {}

  Device& device_;
  BindingLayoutHandle set_layout_;
  bool has_bindless_ = false;  // set 3 present in the layout
  PipelineHandle pipelines_[4] = {};  // [rt | wire]
  PipelineHandle blend_pipelines_[2] = {};  // [rt]
  PipelineHandle prepass_pipeline_;
  // Skinned vertex path: same fragment variants, extra vertex stream + VS.
  PipelineHandle skinned_pipelines_[2] = {};  // [rt]
  PipelineHandle skinned_prepass_pipeline_;
  // Optional mesh-shader opaque variants (larger mesh-stage push range).
  PipelineHandle ms_scene_[2] = {};  // [rt]
  PipelineHandle ms_prepass_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_MESH_PIPELINE_H_

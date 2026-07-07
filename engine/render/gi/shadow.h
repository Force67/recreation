#ifndef RECREATION_RENDER_SHADOW_H_
#define RECREATION_RENDER_SHADOW_H_

#include <functional>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

// Cascaded shadow maps: the raster sun-shadow path for devices and tiers
// without ray tracing. Fits a few orthographic cascades to slices of the camera
// frustum (bounding-sphere fit for rotation stability) and renders opaque depth
// into one wide atlas, cascades laid side by side. mesh.ps samples it with a
// comparison sampler and pcf. The atlas itself is a transient graph texture so
// the render graph owns its barriers and cross-frame recycling; this class owns
// the depth-only pipeline, the cascade fit, and the per-frame cascade ubo.
class ShadowPass {
 public:
  static constexpr u32 kMaxCascades = 4;
  // D16 is enough for the tightened per-cascade ortho ranges and halves the
  // atlas bandwidth; the graph texture and the pipelines must agree.
  static constexpr Format kAtlasFormat = Format::kD16Unorm;

  // Mirrors CascadeData in mesh.ps; 4 light matrices then two param vectors.
  struct CascadeData {
    Mat4 light_view_proj[kMaxCascades];
    f32 p0[4];  // x cascade count, y depth bias, z 1/count, w atlas inset
    f32 p1[4];  // x texel_x (atlas uv), y texel_y, z normal bias, w unused
  };

  struct Settings {
    u32 cascade_count = 4;
    u32 resolution = 2048;     // per-cascade square
    f32 distance = 160.0f;     // furthest shadowed camera distance, meters
    f32 depth_bias = 0.0016f;  // ndc depth units
    f32 normal_bias = 0.06f;   // world offset along the surface normal
  };

  // material_layout is set 0 here (the mesh pipeline's material set), bound per
  // submesh so the fragment stage can alpha-test masked casters.
  bool Initialize(Device& device, BindingLayoutHandle material_layout);
  void Destroy(Device& device);
  void Configure(const Settings& settings);

  u32 atlas_width() const { return settings_.resolution * settings_.cascade_count; }
  u32 atlas_height() const { return settings_.resolution; }

  // Fits cascades to the camera frustum and uploads the matrices for this frame
  // slot. forward/right/up are the camera basis; sun is the light travel dir.
  void Update(const Vec3& eye, const Vec3& forward, const Vec3& right, const Vec3& up, f32 fov_y,
              f32 aspect, const Vec3& sun_direction, u32 frame_slot);

  // Records the depth-only cascade renders into cmd against the atlas view the
  // graph allocated (already in the depth-target state). draw is invoked once
  // per cascade with that cascade's light_view_proj pushed; the caller emits its
  // opaque draws inside, binding pipeline() for static and skinned_pipeline()
  // for animated casters (both share one binding/push interface).
  void Render(CommandList& cmd, TextureView atlas_view,
              const std::function<void(CommandList&)>& draw);

  const GpuBuffer& cascade_buffer(u32 frame_slot) const { return cascades_[frame_slot]; }
  u64 cascade_buffer_size() const { return sizeof(CascadeData); }
  // masked = alpha-tested caster (binds the discard fragment + material set);
  // opaque casters get the depth-only pipeline (no fragment shader at all), so
  // the discard's presence never costs them early-Z.
  PipelineHandle pipeline(bool masked = true) const {
    return masked || !opaque_pipeline_ ? pipeline_ : opaque_pipeline_;
  }
  PipelineHandle skinned_pipeline(bool masked = true) const {
    return masked || !skinned_opaque_pipeline_ ? skinned_pipeline_ : skinned_opaque_pipeline_;
  }

 private:
  static constexpr u32 kFramesInFlight = 2;

  Settings settings_;
  PipelineHandle pipeline_;
  PipelineHandle skinned_pipeline_;
  PipelineHandle opaque_pipeline_;  // depth-only, no fragment stage
  PipelineHandle skinned_opaque_pipeline_;
  GpuBuffer cascades_[kFramesInFlight];
  CascadeData current_{};
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SHADOW_H_

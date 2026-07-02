#ifndef RECREATION_RENDER_HAIR_STRANDS_H_
#define RECREATION_RENDER_HAIR_STRANDS_H_

// Strand-based hair: individually simulated guide strands (verlet particles
// with inextensibility constraints, gravity, wind and head-sphere collision,
// one compute thread per strand) rendered as camera-facing ribbons expanded
// in the vertex shader straight from the simulation buffer, shaded with a
// dual-lobe Kajiya-Kay specular along the strand tangent. Self-contained
// demo path (--demo strands); real characters plug in by seeding roots from
// scalp geometry instead of the procedural cap.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class HairStrands {
 public:
  static constexpr u32 kPointsPerStrand = 16;

  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);
  bool active() const { return strand_count_ > 0; }

  // Seeds strands on the upper hemisphere of a head sphere.
  void SeedCap(Device& device, const Vec3& head_center, f32 head_radius, u32 strand_count,
               f32 strand_length);

  struct Frame {
    Mat4 view_proj;
    Vec3 camera_pos;
    f32 delta_seconds = 0.016f;
    Vec3 sun_direction;  // travel
    f32 sun_intensity = 3.0f;
    Vec3 sun_color{1, 1, 1};
    f32 time = 0.0f;
    f32 wind[3] = {0.4f, 0.0f, 0.2f};
  };

  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  Extent2D extent, const Frame& frame);

 private:
  PipelineHandle sim_pipeline_;
  PipelineHandle draw_pipeline_;
  GpuBuffer points_;   // pos.xyz + inv_mass, prev.xyz + rest_length
  GpuBuffer indices_;  // ribbon triangle list
  Vec3 head_center_{};
  f32 head_radius_ = 0;
  u32 strand_count_ = 0;
  u32 index_count_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_HAIR_STRANDS_H_

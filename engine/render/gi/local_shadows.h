#ifndef RECREATION_RENDER_LOCAL_SHADOWS_H_
#define RECREATION_RENDER_LOCAL_SHADOWS_H_

// Shadow maps for the clustered local lights: a persistent depth atlas of
// square faces (spot = 1 face, point = 6 cube faces at 95 degrees so the
// sampling inset never leaves valid data). Each frame the highest-scoring
// casters near the camera claim faces and re-render with a light-radius
// culled draw list - cheap at local ranges, and cache-friendly to add later.
// The claimed light's params.w carries 1 + its first face index (0 =
// unshadowed), so the shader side needs no extra per-light buffer.

#include <functional>

#include "core/math.h"
#include "render/pipeline/mesh_pipeline.h"
#include "render/rhi/device.h"

namespace rec::render {

class LocalShadows {
 public:
  static constexpr u32 kFaceRes = 512;
  static constexpr u32 kFacesX = 4;
  static constexpr u32 kFacesY = 4;
  static constexpr u32 kMaxFaces = kFacesX * kFacesY;

  // Mirrors LocalShadowFace in mesh.ps.
  struct FaceData {
    Mat4 view_proj;
    f32 rect[4];  // atlas uv scale.xy offset.zw
  };

  // The world-side info the render callback culls against.
  struct Face {
    Mat4 view_proj;
    Vec3 light_pos;
    f32 light_radius = 0;
    u32 slot = 0;  // atlas grid slot
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Scores shadow-casting point/spot lights, claims atlas faces for the best,
  // writes 1 + first face into each claimed light's params.w (0 otherwise)
  // and uploads the face matrices for this frame slot.
  void Assign(PointLight* lights, u32 count, const Vec3& camera, u32 frame_slot);

  // Records the depth renders: one clear + one viewport/scissor per assigned
  // face, the light matrix pushed like the cascade path, draw invoked per
  // face for the caller's culled submissions. Handles the atlas transitions
  // (persistent image: shader-read between frames).
  void Render(CommandList& cmd, PipelineHandle pipeline,
              const std::function<void(CommandList&, const Face&)>& draw);

  u32 face_count() const { return face_count_; }
  const GpuImage& atlas() const { return atlas_; }
  const GpuBuffer& face_buffer(u32 frame_slot) const { return face_buffers_[frame_slot]; }
  u64 face_buffer_size() const { return kMaxFaces * sizeof(FaceData); }

 private:
  static constexpr u32 kFramesInFlight = 2;

  GpuImage atlas_;
  GpuBuffer face_buffers_[kFramesInFlight];
  Face faces_[kMaxFaces];
  u32 face_count_ = 0;
  bool atlas_initialized_ = false;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_LOCAL_SHADOWS_H_

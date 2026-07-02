#include "render/gi/local_shadows.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"

namespace rec::render {
namespace {

// Beyond this camera distance a light never claims a face: its shadow would
// cover a couple of pixels and the face is better spent nearby.
constexpr f32 kMaxShadowDistance = 60.0f;

// Matches the renderer's frame light cap; only used to size the stack array.
constexpr u32 kMaxCandidates = 256;

// Cube faces in +x -x +y -y +z -z order; CubeFaceIndex in mesh.ps must match.
const Vec3 kFaceDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const Vec3 kFaceUps[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};

}  // namespace

bool LocalShadows::Initialize(Device& device) {
  atlas_ = device.CreateImage2D(Format::kD32Float, {kFacesX * kFaceRes, kFacesY * kFaceRes},
                                kTextureUsageDepthTarget | kTextureUsageSampled);
  if (!atlas_) {
    REC_ERROR("local shadow atlas creation failed");
    return false;
  }
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    face_buffers_[i] = device.CreateBuffer(face_buffer_size(), kBufferUsageStorage, true);
    if (!face_buffers_[i].mapped) return false;
  }
  return true;
}

void LocalShadows::Destroy(Device& device) {
  if (atlas_) device.DestroyImage(atlas_);
  atlas_ = {};
  for (GpuBuffer& buffer : face_buffers_) {
    if (buffer) device.DestroyBuffer(buffer);
    buffer = {};
  }
}

void LocalShadows::Assign(PointLight* lights, u32 count, const Vec3& camera, u32 frame_slot) {
  struct Candidate {
    u32 index;
    f32 score;
    u32 faces;
  };
  Candidate candidates[kMaxCandidates];
  u32 candidate_count = 0;

  for (u32 i = 0; i < count; ++i) {
    PointLight& light = lights[i];
    light.params[3] = 0.0f;  // unshadowed until a face is claimed
    u32 type = static_cast<u32>(light.direction_type[3] + 0.5f);
    if (type > 1) continue;  // area lights stay unshadowed (todolist.md)
    f32 radius = light.pos_radius[3];
    if (radius <= 0.0f) continue;
    f32 dx = light.pos_radius[0] - camera.x;
    f32 dy = light.pos_radius[1] - camera.y;
    f32 dz = light.pos_radius[2] - camera.z;
    f32 dist2 = dx * dx + dy * dy + dz * dz;
    if (dist2 > kMaxShadowDistance * kMaxShadowDistance) continue;
    candidates[candidate_count++] = {
        i, light.color_intensity[3] * radius * radius / std::max(dist2, 1.0f),
        type == 0 ? 6u : 1u};
    if (candidate_count == kMaxCandidates) break;
  }
  std::sort(candidates, candidates + candidate_count,
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

  face_count_ = 0;
  FaceData* upload = static_cast<FaceData*>(face_buffers_[frame_slot].mapped);
  for (u32 c = 0; c < candidate_count; ++c) {
    const Candidate& cand = candidates[c];
    if (face_count_ + cand.faces > kMaxFaces) continue;  // a smaller light may still fit
    PointLight& light = lights[cand.index];
    light.params[3] = static_cast<f32>(face_count_ + 1);

    Vec3 pos{light.pos_radius[0], light.pos_radius[1], light.pos_radius[2]};
    f32 far = std::max(light.pos_radius[3], 0.25f);
    u32 type = static_cast<u32>(light.direction_type[3] + 0.5f);
    for (u32 f = 0; f < cand.faces; ++f) {
      Mat4 view;
      f32 fov;
      if (type == 1) {  // spot: one face down the cone, slightly overscanned
        Vec3 dir = Normalize(Vec3{light.direction_type[0], light.direction_type[1],
                                  light.direction_type[2]});
        Vec3 up = std::abs(dir.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        view = LookAt(pos, pos + dir, up);
        f32 outer = std::acos(std::clamp(light.params[1], -1.0f, 1.0f));
        fov = std::clamp(2.2f * outer, 0.2f, 2.9f);
      } else {  // point: 95-degree cube faces so the sampling inset stays valid
        view = LookAt(pos, pos + kFaceDirs[f], kFaceUps[f]);
        fov = 1.658f;
      }
      Mat4 vp = PerspectiveShadow(fov, 1.0f, 0.05f, far) * view;

      u32 slot = face_count_;
      Face& face = faces_[slot];
      face.view_proj = vp;
      face.light_pos = pos;
      face.light_radius = far;
      face.slot = slot;

      FaceData& data = upload[slot];
      data.view_proj = vp;
      data.rect[0] = 1.0f / kFacesX;
      data.rect[1] = 1.0f / kFacesY;
      data.rect[2] = static_cast<f32>(slot % kFacesX) / kFacesX;
      data.rect[3] = static_cast<f32>(slot / kFacesX) / kFacesY;
      ++face_count_;
    }
  }
}

void LocalShadows::Render(CommandList& cmd, PipelineHandle pipeline,
                          const std::function<void(CommandList&, const Face&)>& draw) {
  // Persistent atlas: shader-read between frames, depth target while writing.
  TextureBarrier to_write = Transition(
      atlas_, atlas_initialized_ ? ResourceState::kShaderReadFragment : ResourceState::kUndefined,
      ResourceState::kDepthTarget);
  atlas_initialized_ = true;
  cmd.TextureBarriers({&to_write, 1});

  DepthAttachment depth{
      .view = atlas_.view, .load = LoadOp::kClear, .store = StoreOp::kStore, .clear = 1.0f};
  cmd.BeginRendering({.extent = atlas_.extent, .depth = &depth});
  cmd.BindPipeline(pipeline);  // push constants resolve against the bound pipeline
  for (u32 i = 0; i < face_count_; ++i) {
    const Face& face = faces_[i];
    f32 x = static_cast<f32>((face.slot % kFacesX) * kFaceRes);
    f32 y = static_cast<f32>((face.slot / kFacesX) * kFaceRes);
    cmd.SetViewport(x, y, kFaceRes, kFaceRes);
    cmd.SetScissor(static_cast<i32>(x), static_cast<i32>(y), kFaceRes, kFaceRes);
    cmd.PushConstants(&face.view_proj, sizeof(Mat4));
    draw(cmd, face);
  }
  cmd.EndRendering();

  TextureBarrier to_read =
      Transition(atlas_, ResourceState::kDepthTarget, ResourceState::kShaderReadFragment);
  cmd.TextureBarriers({&to_read, 1});
}

}  // namespace rec::render

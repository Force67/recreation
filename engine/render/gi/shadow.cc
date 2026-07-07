#include "render/gi/shadow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include "asset/mesh.h"
#include "core/log.h"
#include "shaders/shadow_ps_hlsl.h"
#include "shaders/shadow_skin_vs_hlsl.h"
#include "shaders/shadow_vs_hlsl.h"

namespace rec::render {
namespace {

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Mul(const Vec3& v, f32 s) { return {v.x * s, v.y * s, v.z * s}; }

}  // namespace

bool ShadowPass::Initialize(Device& device, BindingLayoutHandle material_layout) {
  // Position (binding 0) + uv for alpha test; the skinned variant adds the bone
  // index/weight stream (binding 1) so it skins in the vertex stage. The push
  // block covers the skinned permutation's trailing bone_address + skin_offset;
  // the static caster only writes the two leading matrices.
  VertexBufferLayout position_stream{
      .stride = sizeof(asset::Vertex),
      .attributes = {{0, Format::kRGB32Float, offsetof(asset::Vertex, position)},
                     {3, Format::kRG32Float, offsetof(asset::Vertex, uv)}}};
  VertexBufferLayout skin_stream{
      .stride = sizeof(asset::SkinnedVertexExtra),
      .attributes = {{5, Format::kRGBA8Uint, offsetof(asset::SkinnedVertexExtra, bone_indices)},
                     {6, Format::kRGBA8Unorm, offsetof(asset::SkinnedVertexExtra, bone_weights)}}};

  auto make_pipeline = [&](ShaderBlob vertex, bool skinned, bool masked, const char* name) {
    GraphicsPipelineDesc desc{
        .vertex = vertex,
        // Masked casters alpha-test in the fragment; opaque ones run with no
        // fragment shader at all (the discard's presence would force late-Z).
        .fragment = masked ? REC_SHADER(k_shadow_ps_hlsl) : ShaderBlob{},
        // Thin geometry must cast from both sides.
        .raster = {.cull = CullMode::kNone, .front = FrontFace::kCounterClockwise},
        // Standard depth, nearest occluder wins; slope-scaled bias kills most
        // shadow acne. D16: each cascade's ortho range is tightened to its
        // frustum slice, where 16 bits resolve fine and cost half the
        // bandwidth of D32 (shadow pixel cost scales with resolution^2).
        .depth = {.test = true,
                  .write = true,
                  .compare = CompareOp::kLess,
                  .format = kAtlasFormat,
                  .bias_constant = 1.25f,
                  .bias_slope = 2.0f},
        .sets = {{.shared = material_layout}},  // set 0: alpha-test inputs
        .push_constant_size = 2 * sizeof(Mat4) + 16,
        .debug_name = name,
    };
    desc.vertex_buffers.push_back(position_stream);
    if (skinned) desc.vertex_buffers.push_back(skin_stream);
    return device.CreateGraphicsPipeline(desc);
  };

  pipeline_ = make_pipeline(REC_SHADER(k_shadow_vs_hlsl), false, true, "shadow");
  skinned_pipeline_ =
      make_pipeline(REC_SHADER(k_shadow_skin_vs_hlsl), true, true, "shadow_skinned");
  opaque_pipeline_ = make_pipeline(REC_SHADER(k_shadow_vs_hlsl), false, false, "shadow_opaque");
  skinned_opaque_pipeline_ =
      make_pipeline(REC_SHADER(k_shadow_skin_vs_hlsl), true, false, "shadow_skinned_opaque");
  if (!pipeline_ || !skinned_pipeline_ || !opaque_pipeline_ || !skinned_opaque_pipeline_) {
    REC_ERROR("shadow pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    cascades_[i] = device.CreateBuffer(sizeof(CascadeData), kBufferUsageUniform, true);
    if (!cascades_[i].mapped) return false;
    std::memset(cascades_[i].mapped, 0, sizeof(CascadeData));
  }
  return true;
}

void ShadowPass::Configure(const Settings& settings) {
  settings_ = settings;
  settings_.cascade_count = std::clamp(settings_.cascade_count, 1u, kMaxCascades);
}

void ShadowPass::Update(const Vec3& eye, const Vec3& forward, const Vec3& right, const Vec3& up,
                        f32 fov_y, f32 aspect, const Vec3& sun_direction, u32 frame_slot) {
  const u32 count = settings_.cascade_count;
  const f32 near_plane = 0.1f;
  const f32 far_plane = settings_.distance;
  const f32 tan_half = std::tan(fov_y * 0.5f);
  const f32 lambda = 0.7f;       // log/uniform split blend
  const f32 back_pad = 80.0f;    // caster range behind the slice, toward the sun

  f32 splits[kMaxCascades + 1];
  splits[0] = near_plane;
  for (u32 i = 1; i <= count; ++i) {
    f32 p = static_cast<f32>(i) / static_cast<f32>(count);
    f32 log_split = near_plane * std::pow(far_plane / near_plane, p);
    f32 uniform_split = near_plane + (far_plane - near_plane) * p;
    splits[i] = lambda * log_split + (1.0f - lambda) * uniform_split;
  }

  Vec3 light_dir = Normalize(sun_direction);  // travel direction = look direction
  Vec3 up_ref = std::abs(light_dir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};

  current_ = CascadeData{};
  for (u32 i = 0; i < count; ++i) {
    f32 cn = splits[i], cf = splits[i + 1];
    Vec3 corners[8];
    u32 c = 0;
    for (f32 d : {cn, cf}) {
      f32 hh = d * tan_half;
      f32 hw = hh * aspect;
      Vec3 center_d = Add(eye, Mul(forward, d));
      for (f32 sx : {-1.0f, 1.0f})
        for (f32 sy : {-1.0f, 1.0f})
          corners[c++] = Add(center_d, Add(Mul(right, hw * sx), Mul(up, hh * sy)));
    }

    Vec3 center{0, 0, 0};
    for (const Vec3& p : corners) center = Add(center, Mul(p, 1.0f / 8.0f));
    f32 radius = 0.0f;
    for (const Vec3& p : corners) {
      Vec3 v = {p.x - center.x, p.y - center.y, p.z - center.z};
      radius = std::max(radius, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;  // quantize so it stops pulsing

    Vec3 light_eye = {center.x - light_dir.x * (radius + back_pad),
                      center.y - light_dir.y * (radius + back_pad),
                      center.z - light_dir.z * (radius + back_pad)};
    Mat4 light_view = LookAt(light_eye, center, up_ref);
    Mat4 light_proj =
        Orthographic(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + back_pad);
    Mat4 light_vp = light_proj * light_view;

    // Texel snap: round the projected world origin to whole shadow texels so the
    // cascade slides in texel steps and stops shimmering as the camera moves.
    Vec3 origin_ndc = TransformPoint(light_vp, Vec3{0, 0, 0});
    f32 half_res = settings_.resolution * 0.5f;
    f32 sx = origin_ndc.x * half_res;
    f32 sy = origin_ndc.y * half_res;
    f32 dx = (std::round(sx) - sx) / half_res;
    f32 dy = (std::round(sy) - sy) / half_res;
    light_vp.m[12] += dx;
    light_vp.m[13] += dy;

    current_.light_view_proj[i] = light_vp;
  }

  current_.p0[0] = static_cast<f32>(count);
  current_.p0[1] = settings_.depth_bias;
  current_.p0[2] = 1.0f / static_cast<f32>(count);
  current_.p0[3] = 1.5f / static_cast<f32>(settings_.resolution);  // inset, a few texels
  current_.p1[0] = 1.0f / static_cast<f32>(settings_.resolution);  // cascade-local texel
  current_.p1[1] = 0.0f;
  current_.p1[2] = settings_.normal_bias;
  current_.p1[3] = 0.0f;
  std::memcpy(cascades_[frame_slot].mapped, &current_, sizeof(CascadeData));
}

void ShadowPass::Render(CommandList& cmd, TextureView atlas_view,
                        const std::function<void(CommandList&)>& draw) {
  const u32 res = settings_.resolution;

  // The graph already put the atlas in the depth-target state for this write.
  DepthAttachment depth{
      .view = atlas_view, .load = LoadOp::kClear, .store = StoreOp::kStore, .clear = 1.0f};
  cmd.BeginRendering({.extent = {res * settings_.cascade_count, res}, .depth = &depth});
  // Push constants resolve against the bound pipeline, so bind the static
  // permutation up front; the draw callback binds pipeline()/skinned_pipeline()
  // per mesh and both share the same push/set interface, so the per-cascade
  // light matrix push below stays valid.
  cmd.BindPipeline(pipeline_);

  for (u32 i = 0; i < settings_.cascade_count; ++i) {
    cmd.SetViewport(static_cast<f32>(i * res), 0.0f, static_cast<f32>(res), static_cast<f32>(res));
    cmd.SetScissor(static_cast<i32>(i * res), 0, res, res);
    cmd.PushConstants(&current_.light_view_proj[i], sizeof(Mat4));
    draw(cmd);
  }
  cmd.EndRendering();
}

void ShadowPass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  device.DestroyPipeline(skinned_pipeline_);
  device.DestroyPipeline(opaque_pipeline_);
  device.DestroyPipeline(skinned_opaque_pipeline_);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(cascades_[i]);
  pipeline_ = {};
  skinned_pipeline_ = {};
}

}  // namespace rec::render

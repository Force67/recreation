// kinema unit tests: synthetic data only, no game assets. Exits non-zero on
// the first failure so it slots into ctest.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "kinema/kinema.h"

namespace {

using namespace kinema;

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

Quat AxisAngle(f32 x, f32 y, f32 z, f32 angle) {
  f32 len = std::sqrt(x * x + y * y + z * z);
  f32 s = std::sin(angle * 0.5f) / (len > 0 ? len : 1.0f);
  return Quat{x * s, y * s, z * s, std::cos(angle * 0.5f)};
}

f32 QuatError(const Quat& a, const Quat& b) {
  f32 dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 1.0f - std::min(dot, 1.0f);  // 0 = identical orientation
}

// Round-trip: analytic tracks through the builder and sampler.
void TestClipRoundTrip() {
  constexpr u32 kTracks = 8, kFrames = 61;
  constexpr f32 kRate = 30.0f;
  ClipBuilder builder(kTracks, kFrames, kRate);
  auto truth = [](u32 track, f32 time, Vec3* t, Quat* r, f32* s) {
    if (track == 0) {  // constant track
      *t = Vec3{1, 2, 3};
      *r = AxisAngle(0, 0, 1, 0.5f);
      *s = 1.0f;
      return;
    }
    f32 phase = time * (1.0f + 0.3f * static_cast<f32>(track));
    *t = Vec3{50.0f * std::sin(phase), 10.0f * static_cast<f32>(track),
              25.0f * std::cos(phase * 0.7f)};
    *r = AxisAngle(0.2f, 1, 0.1f * static_cast<f32>(track), 2.0f * std::sin(phase * 0.5f));
    *s = 1.0f + 0.25f * std::sin(phase);
  };
  for (u32 f = 0; f < kFrames; ++f) {
    for (u32 track = 0; track < kTracks; ++track) {
      Vec3 t;
      Quat r;
      f32 s;
      truth(track, static_cast<f32>(f) / kRate, &t, &r, &s);
      builder.SetSample(f, track, t, r, s);
    }
  }
  builder.AddEvent("FootLeft", 0.4f);
  builder.AddEvent("FootRight", 1.1f);
  builder.AddRootKey(2.0f, Vec3{0, 100, 0});
  OwnedClip clip(builder.Build());
  CHECK(static_cast<bool>(clip));
  CHECK(clip->num_tracks() == kTracks);
  CHECK_NEAR(clip->duration(), (kFrames - 1) / kRate, 1e-5f);

  PoseArena arena(kTracks, 1);
  PoseView pose = arena.Acquire();
  f32 worst_t = 0, worst_q = 0, worst_s = 0;
  for (int i = 0; i <= 200; ++i) {
    f32 time = clip->duration() * static_cast<f32>(i) / 200.0f;
    clip->Sample(time, pose);
    // Ground truth is the piecewise-linear interpolation of the uniform
    // samples — exactly what the codec stores — so the deltas below measure
    // pure quantization error, not source-curve linearization.
    f32 x = time * kRate;
    u32 k = std::min(static_cast<u32>(x), kFrames - 2);
    f32 a = x - static_cast<f32>(k);
    for (u32 track = 0; track < kTracks; ++track) {
      Vec3 t0, t1;
      Quat r0, r1;
      f32 s0, s1;
      truth(track, static_cast<f32>(k) / kRate, &t0, &r0, &s0);
      truth(track, static_cast<f32>(k + 1) / kRate, &t1, &r1, &s1);
      Vec3 t{t0.x + (t1.x - t0.x) * a, t0.y + (t1.y - t0.y) * a, t0.z + (t1.z - t0.z) * a};
      f32 s = s0 + (s1 - s0) * a;
      worst_t = std::max({worst_t, std::abs(pose.translation[track].x - t.x),
                          std::abs(pose.translation[track].y - t.y),
                          std::abs(pose.translation[track].z - t.z)});
      // Quat ground truth at key times only (nlerp between differs from the
      // codec's component lerp by < quantization for adjacent frames).
      if (a < 1e-4f) worst_q = std::max(worst_q, QuatError(pose.rotation[track], r0));
      worst_s = std::max(worst_s, std::abs(pose.scale[track] - s));
    }
  }
  CHECK(worst_t < 0.005f);  // 16-bit over a 100-unit range
  CHECK(worst_q < 1e-4f);
  CHECK(worst_s < 0.001f);

  // Constant track must be exact (stored full precision).
  clip->Sample(1.234f, pose);
  CHECK(pose.translation[0].x == 1.0f && pose.translation[0].y == 2.0f);

  // Events: range, boundary and wrap semantics.
  int hits = 0;
  clip->EventsInRange(0.0f, 0.5f, [&](const ClipEvent& e) {
    ++hits;
    CHECK(e.name_hash == HashName("FootLeft"));
  });
  CHECK(hits == 1);
  hits = 0;
  clip->EventsInRange(1.5f, 0.5f, [&](const ClipEvent&) { ++hits; });  // wrap
  CHECK(hits == 1);

  // Root motion: linear ramp, delta with wrap.
  Vec3 half = clip->RootTranslation(1.0f);
  CHECK_NEAR(half.y, 50.0f, 1e-3f);
  Vec3 wrap = clip->RootDelta(1.5f, 0.5f);
  CHECK_NEAR(wrap.y, 50.0f, 1.0f);  // 25 to end + 25 past start (duration 2s)

  // Blob survives a copy (relocatable).
  std::vector<u8> copy(clip.bytes());
  auto view = Clip::FromBlob(copy.data(), copy.size());
  CHECK(view.has_value());
  view->Sample(0.5f, pose);
}

void TestBlendKernels() {
  constexpr u32 kBones = 4;
  PoseArena arena(kBones, 4);
  PoseView a = arena.Acquire(), b = arena.Acquire(), dst = arena.Acquire();
  for (u32 i = 0; i < kBones; ++i) {
    a.translation[i] = Vec3{0, 0, 0};
    a.rotation[i] = Quat{};
    a.scale[i] = 1;
    b.translation[i] = Vec3{2, 4, 6};
    b.rotation[i] = AxisAngle(0, 1, 0, 1.0f);
    b.scale[i] = 3;
  }
  BlendPoses(a, b, 0.5f, dst);
  CHECK_NEAR(dst.translation[0].y, 2.0f, 1e-6f);
  CHECK_NEAR(dst.scale[0], 2.0f, 1e-6f);
  CHECK_NEAR(QuatError(dst.rotation[0], AxisAngle(0, 1, 0, 0.5f)), 0.0f, 1e-4f);

  // Double cover: blending toward -q must not go the long way round.
  Quat neg{-b.rotation[0].x, -b.rotation[0].y, -b.rotation[0].z, -b.rotation[0].w};
  for (u32 i = 0; i < kBones; ++i) b.rotation[i] = neg;
  BlendPoses(a, b, 0.5f, dst);
  CHECK_NEAR(QuatError(dst.rotation[0], AxisAngle(0, 1, 0, 0.5f)), 0.0f, 1e-4f);

  // Additive identity at weight 0; full application at weight 1.
  for (u32 i = 0; i < kBones; ++i) {
    b.translation[i] = Vec3{1, 0, 0};
    b.rotation[i] = AxisAngle(1, 0, 0, 0.4f);
    b.scale[i] = 1.5f;
  }
  ApplyAdditive(a, b, 0.0f, dst);
  CHECK_NEAR(dst.translation[0].x, 0.0f, 1e-6f);
  CHECK_NEAR(QuatError(dst.rotation[0], Quat{}), 0.0f, 1e-6f);
  ApplyAdditive(a, b, 1.0f, dst);
  CHECK_NEAR(dst.translation[0].x, 1.0f, 1e-6f);
  CHECK_NEAR(QuatError(dst.rotation[0], AxisAngle(1, 0, 0, 0.4f)), 0.0f, 1e-4f);

  // Masked blend: bone 1 pinned to a.
  f32 mask[kBones] = {1, 0, 1, 1};
  for (u32 i = 0; i < kBones; ++i) b.translation[i] = Vec3{2, 4, 6};
  BlendPosesMasked(a, b, 1.0f, mask, dst);
  CHECK_NEAR(dst.translation[0].y, 4.0f, 1e-6f);
  CHECK_NEAR(dst.translation[1].y, 0.0f, 1e-6f);
}

void TestProgram() {
  constexpr u32 kTracks = 2;
  ClipBuilder ba(kTracks, 2, 30.0f), bb(kTracks, 2, 30.0f);
  for (u32 f = 0; f < 2; ++f) {
    for (u32 t = 0; t < kTracks; ++t) {
      ba.SetSample(f, t, Vec3{0, 0, 0}, Quat{}, 1);
      bb.SetSample(f, t, Vec3{10, 0, 0}, Quat{}, 1);
    }
  }
  OwnedClip ca(ba.Build()), cb(bb.Build());
  PoseArena arena(kTracks, 3);
  PoseOp ops[3];
  ops[0] = {.kind = PoseOp::Kind::kSample, .dst = 0, .clip = ca.get(), .time = 0};
  ops[1] = {.kind = PoseOp::Kind::kSample, .dst = 1, .clip = cb.get(), .time = 0};
  ops[2] = {.kind = PoseOp::Kind::kBlend, .dst = 2, .a = 0, .b = 1, .alpha = 0.25f};
  PoseView out = ExecuteProgram(ops, 3, arena);
  CHECK_NEAR(out.translation[0].x, 2.5f, 1e-5f);
}

void TestInertializer() {
  constexpr u32 kBones = 2;
  PoseArena arena(kBones, 3);
  PoseView from = arena.Acquire(), to = arena.Acquire(), pose = arena.Acquire();
  for (u32 i = 0; i < kBones; ++i) {
    from.translation[i] = Vec3{1, 0, 0};
    from.rotation[i] = AxisAngle(0, 0, 1, 0.6f);
    from.scale[i] = 2;
    to.translation[i] = Vec3{0, 0, 0};
    to.rotation[i] = Quat{};
    to.scale[i] = 1;
  }
  Inertializer inert;
  inert.Init(kBones);
  inert.Begin(from, to, 0.25f);
  // Immediately after the switch the offset restores the old pose...
  CopyPose(to, pose);
  inert.Apply(pose, 0.0f);
  CHECK_NEAR(pose.translation[0].x, 1.0f, 1e-4f);
  CHECK_NEAR(QuatError(pose.rotation[0], from.rotation[0]), 0.0f, 1e-4f);
  // ...decays monotonically...
  f32 prev = 1.0f;
  for (int i = 0; i < 10; ++i) {
    CopyPose(to, pose);
    inert.Apply(pose, 0.025f);
    CHECK(pose.translation[0].x <= prev + 1e-5f);
    prev = pose.translation[0].x;
  }
  // ...and lands exactly on the new pose.
  CopyPose(to, pose);
  bool active = inert.Apply(pose, 1.0f);
  CHECK(!active);
  CHECK_NEAR(pose.translation[0].x, 0.0f, 1e-5f);
}

void TestModelSpace() {
  Skeleton skel;
  skel.parents = {-1, 0, 1};
  skel.name_hashes = {HashName("root"), HashName("mid"), HashName("tip")};
  skel.bind_translation.resize(3);
  skel.bind_rotation.resize(3);
  skel.bind_scale.assign(3, 1.0f);
  PoseArena arena(3, 1);
  PoseView local = arena.Acquire();
  local.translation[0] = Vec3{0, 1, 0};
  local.rotation[0] = AxisAngle(0, 0, 1, 1.57079633f);  // +90 deg about z
  local.scale[0] = 1;
  local.translation[1] = Vec3{1, 0, 0};
  local.rotation[1] = Quat{};
  local.scale[1] = 2;
  local.translation[2] = Vec3{1, 0, 0};
  local.rotation[2] = Quat{};
  local.scale[2] = 1;
  Vec3 mt[3];
  Quat mr[3];
  f32 ms[3];
  ComputeModelSpace(skel, local, mt, mr, ms);
  // mid: root rotates +x into +y.
  CHECK_NEAR(mt[1].x, 0.0f, 1e-5f);
  CHECK_NEAR(mt[1].y, 2.0f, 1e-5f);
  // tip: mid's scale 2 stretches its child offset, still rotated into +y.
  CHECK_NEAR(mt[2].y, 4.0f, 1e-5f);
  CHECK_NEAR(ms[2], 2.0f, 1e-6f);
  CHECK(skel.Find(HashName("tip")) == 2);
}

}  // namespace

int main() {
  TestClipRoundTrip();
  TestBlendKernels();
  TestProgram();
  TestInertializer();
  TestModelSpace();
  if (failures == 0) {
    std::printf("kinematest: all passed\n");
    return 0;
  }
  std::printf("kinematest: %d failures\n", failures);
  return 1;
}

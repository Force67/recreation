// hkxinfo: inspect Havok packfiles (.hkx) from loose files or Skyrim BSAs.
//
//   hkxinfo <file.hkx> [mode...]
//   hkxinfo --data <dir> <internal/path.hkx> [mode...]
//
// Modes: --sections (default), --objects, --classes, --extract <out.hkx>,
//        --hex <offset> [count]
//
// Data-dependent (real Skyrim archives); not run in CI.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "bethesda/hkx.h"
#include "bethesda/hkx_physics.h"
#include "bethesda/hkx_to_physics.h"
#include "bethesda/hkx_anim.h"
#include "bethesda/hkx_character.h"
#include "bethesda/hkx_to_kinema.h"
#include <chrono>
#include <kinema/kinema.h>
#include "physics/physics_world.h"
#include <cmath>

namespace {

using rec::bethesda::HkxFile;

// Transcodes an animation to kinema and compares against the reference
// spline sampler at `samples` deterministic pseudo-random times. Returns the
// worst deltas through the out params; false when the file has no animation.
bool CompareKinema(const rec::bethesda::HkxAnimation& anim, int samples, float* worst_t,
                   float* worst_q, float* worst_s, size_t* blob_size) {
  auto blob = rec::bethesda::TranscodeToKinema(anim, nullptr, nullptr);
  auto clip = kinema::Clip::FromBlob(blob.data(), blob.size());
  if (!clip) return false;
  *blob_size = blob.size();
  kinema::PoseArena arena(anim.num_tracks, 1);
  kinema::PoseView pose = arena.Acquire();
  std::vector<rec::bethesda::HkxTrackPose> reference;
  rec::u32 seed = 0x9E3779B9u;
  *worst_t = *worst_q = *worst_s = 0;
  for (int i = 0; i < samples; ++i) {
    seed = seed * 1664525u + 1013904223u;
    float time = anim.duration * static_cast<float>(seed >> 8) / 16777216.0f;
    rec::bethesda::SampleAnimation(anim, time, &reference);
    clip->Sample(time, pose);
    for (rec::u32 t = 0; t < anim.num_tracks && t < reference.size(); ++t) {
      const auto& r = reference[t];
      // A handful of vanilla clips carry authored-garbage position ranges
      // (1e15+ game units on unused 1st-person helper tracks); both samplers
      // reproduce the garbage but relative float error explodes, so exclude
      // anything far outside plausible authored space from the metric.
      if (std::abs(r.translation.x) > 1e6f || std::abs(r.translation.y) > 1e6f ||
          std::abs(r.translation.z) > 1e6f) {
        continue;
      }
      *worst_t = std::max({*worst_t, std::abs(pose.translation[t].x - r.translation.x),
                           std::abs(pose.translation[t].y - r.translation.y),
                           std::abs(pose.translation[t].z - r.translation.z)});
      float dot = std::abs(pose.rotation[t].x * r.rotation[0] + pose.rotation[t].y * r.rotation[1] +
                           pose.rotation[t].z * r.rotation[2] + pose.rotation[t].w * r.rotation[3]);
      *worst_q = std::max(*worst_q, 1.0f - std::min(dot, 1.0f));
      *worst_s = std::max(*worst_s, std::abs(pose.scale[t] - r.scale));
    }
  }
  return true;
}

std::vector<rec::u8> ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<rec::u8>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

void PrintObjects(const HkxFile& hkx) {
  std::printf("%zu objects:\n", hkx.objects().size());
  for (const auto& obj : hkx.objects()) {
    std::printf("  %08llx %.*s\n", static_cast<unsigned long long>(obj.offset),
                static_cast<int>(obj.class_name.size()), obj.class_name.data());
  }
}

void PrintClasses(const HkxFile& hkx) {
  std::map<std::string, int> histogram;
  for (const auto& obj : hkx.objects()) histogram[std::string(obj.class_name)]++;
  for (const auto& [name, count] : histogram) {
    std::printf("  %4d %s\n", count, name.c_str());
  }
}

void PrintHex(const HkxFile& hkx, rec::u64 offset, rec::u64 count) {
  for (rec::u64 row = 0; row < count; row += 16) {
    std::printf("%08llx ", static_cast<unsigned long long>(offset + row));
    for (rec::u64 i = 0; i < 16 && offset + row + i < hkx.data_size(); ++i) {
      std::printf("%02x%s", hkx.data()[offset + row + i], (i % 4 == 3) ? " " : "");
    }
    // Pointer / float annotations per 8 bytes.
    std::printf(" |");
    for (rec::u64 i = 0; i < 16; i += 8) {
      rec::u64 at = offset + row + i;
      rec::u64 target = hkx.Pointer(at);
      if (target != HkxFile::kNull) {
        std::string_view cls = hkx.class_of(target);
        std::printf(" ->%llx%s%.*s", static_cast<unsigned long long>(target),
                    cls.empty() ? "" : ":", static_cast<int>(cls.size()), cls.data());
      }
    }
    std::printf(" | %g %g %g %g\n", hkx.F32(offset + row), hkx.F32(offset + row + 4),
                hkx.F32(offset + row + 8), hkx.F32(offset + row + 12));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    std::fprintf(stderr, "usage: hkxinfo <file.hkx> | --data <dir> <internal/path> [modes]\n");
    return 1;
  }

  std::vector<rec::u8> bytes;
  size_t consumed = 0;
  if (args[0] == "--data") {
    if (args.size() < 3) {
      std::fprintf(stderr, "--data needs <dir> <internal/path>\n");
      return 1;
    }
    rec::asset::Vfs vfs;
    for (const auto& entry : std::filesystem::directory_iterator(args[1])) {
      const auto ext = entry.path().extension();
      if (ext == ".bsa" || ext == ".ba2") {  // Skyrim / Fallout 4+ archives
        if (auto provider = rec::bethesda::OpenArchive(entry.path().string())) {
          vfs.Mount(std::move(provider));
        }
      }
    }
    std::printf("mounted %zu archives\n", vfs.mount_count());
    if (args[2].rfind("--list", 0) == 0 && args.size() > 3) {
      // hkxinfo --data <dir> --list <substring>: enumerate matching paths.
      std::string needle = args[3];
      vfs.Enumerate([&](std::string_view path) {
        if (path.find(needle) != std::string_view::npos) {
          std::printf("%.*s\n", static_cast<int>(path.size()), path.data());
        }
      });
      return 0;
    }
    if (args[2].rfind("--scan", 0) == 0 && args.size() > 3) {
      // hkxinfo --data <dir> --scan <substring>: decode every matching .hkx
      // animation and report bindings with an additive blend hint or float
      // tracks (the rare vanilla features worth locating).
      std::string needle = args[3];
      std::vector<std::string> paths;
      vfs.Enumerate([&](std::string_view path) {
        if (path.find(needle) != std::string_view::npos && path.size() > 4 &&
            path.substr(path.size() - 4) == ".hkx") {
          paths.emplace_back(path);
        }
      });
      int scanned = 0, additive = 0;
      for (const std::string& path : paths) {
        auto data = vfs.Read(path);
        if (!data) continue;
        auto file = HkxFile::Parse(data->data(), data->size());
        if (!file) continue;
        auto anim = rec::bethesda::DecodeAnimation(*file);
        if (!anim) continue;
        ++scanned;
        if (anim->additive) {
          ++additive;
          std::printf("ADDITIVE %s (%.2fs, %u tracks)\n", path.c_str(), anim->duration,
                      anim->num_tracks);
        }
      }
      std::printf("scanned %d animations, %d additive\n", scanned, additive);
      return 0;
    }
    if (args[2].rfind("--kinemascan", 0) == 0 && args.size() > 3) {
      // hkxinfo --data <dir> --kinemascan <substring>: transcode every
      // matching animation to kinema and report the worst deviation from the
      // reference spline sampler across the corpus.
      std::string needle = args[3];
      std::vector<std::string> paths;
      vfs.Enumerate([&](std::string_view path) {
        if (path.find(needle) != std::string_view::npos && path.size() > 4 &&
            path.substr(path.size() - 4) == ".hkx") {
          paths.emplace_back(path);
        }
      });
      int scanned = 0, outlier_t = 0, outlier_q = 0;
      float corpus_t = 0, corpus_q = 0, corpus_s = 0;
      size_t total_hkx = 0, total_blob = 0;
      std::string worst_t_file, worst_q_file, worst_s_file;
      for (const std::string& path : paths) {
        auto data = vfs.Read(path);
        if (!data) continue;
        auto file = HkxFile::Parse(data->data(), data->size());
        if (!file) continue;
        auto anim = rec::bethesda::DecodeAnimation(*file);
        if (!anim) continue;
        float wt = 0, wq = 0, ws = 0;
        size_t blob = 0;
        if (!CompareKinema(*anim, 32, &wt, &wq, &ws, &blob)) continue;
        ++scanned;
        total_hkx += data->size();
        total_blob += blob;
        if (wt > corpus_t) worst_t_file = path;
        if (wq > corpus_q) worst_q_file = path;
        if (ws > corpus_s) worst_s_file = path;
        corpus_t = std::max(corpus_t, wt);
        corpus_q = std::max(corpus_q, wq);
        corpus_s = std::max(corpus_s, ws);
        if (wt > 0.1f) ++outlier_t;
        if (wq > 1e-3f) ++outlier_q;
      }
      std::printf("kinema corpus: %d clips\n  worst dt %.5f gu (%s)\n  worst dq %.2e (%s)\n"
                  "  worst ds %.2e (%s)\n  outliers: %d clips dt>0.1gu, %d clips dq>1e-3\n",
                  scanned, corpus_t, worst_t_file.c_str(), corpus_q, worst_q_file.c_str(),
                  corpus_s, worst_s_file.c_str(), outlier_t, outlier_q);
      std::printf("  size: hkx %.1f MiB -> kinema %.1f MiB (%.2fx)\n",
                  static_cast<double>(total_hkx) / 1048576.0,
                  static_cast<double>(total_blob) / 1048576.0,
                  static_cast<double>(total_blob) / static_cast<double>(std::max<size_t>(total_hkx, 1)));
      return 0;
    }
    auto data = vfs.Read(args[2]);
    if (!data) {
      std::fprintf(stderr, "not found in archives: %s\n", args[2].c_str());
      return 1;
    }
    bytes.assign(data->begin(), data->end());
    consumed = 3;
  } else {
    bytes = ReadFileBytes(args[0]);
    if (bytes.empty()) {
      std::fprintf(stderr, "cannot read %s\n", args[0].c_str());
      return 1;
    }
    consumed = 1;
  }

  // Raw extraction works for any VFS file (animationdata .txt etc.), so it
  // runs before the packfile parse.
  for (size_t i = consumed; i + 1 < args.size(); ++i) {
    if (args[i] == "--extract") {
      std::ofstream out(args[i + 1], std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
      std::printf("wrote %s (%zu bytes)\n", args[i + 1].c_str(), bytes.size());
    }
  }

  auto hkx = HkxFile::Parse(bytes.data(), bytes.size());
  if (!hkx) {
    std::fprintf(stderr, "not a supported havok packfile (%zu bytes, magic %02x%02x%02x%02x)\n",
                 bytes.size(), bytes.size() > 0 ? bytes[0] : 0, bytes.size() > 1 ? bytes[1] : 0,
                 bytes.size() > 2 ? bytes[2] : 0, bytes.size() > 3 ? bytes[3] : 0);
    return 1;
  }
  std::printf("havok packfile: %s, %u-bit pointers, data %zu bytes, %zu objects\n",
              hkx->content_version().c_str(), hkx->pointer_size() * 8, hkx->data_size(),
              hkx->objects().size());

  bool any_mode = false;
  for (size_t i = consumed; i < args.size(); ++i) {
    any_mode = true;
    if (args[i] == "--objects") {
      PrintObjects(*hkx);
    } else if (args[i] == "--classes") {
      PrintClasses(*hkx);
    } else if (args[i] == "--extract" && i + 1 < args.size()) {
      ++i;  // handled pre-parse
    } else if (args[i] == "--kinema") {
      // Transcode to kinema, validate against the spline sampler, then race
      // the two samplers.
      auto anim = rec::bethesda::DecodeAnimation(*hkx);
      if (!anim) {
        std::fprintf(stderr, "no decodable spline-compressed animation\n");
        return 1;
      }
      float wt = 0, wq = 0, ws = 0;
      size_t blob_size = 0;
      if (!CompareKinema(*anim, 256, &wt, &wq, &ws, &blob_size)) {
        std::fprintf(stderr, "kinema transcode failed\n");
        return 1;
      }
      std::printf("kinema: %u tracks x %u frames, blob %zu bytes (source hkx %zu)\n",
                  anim->num_tracks, anim->num_frames, blob_size, bytes.size());
      std::printf("  vs spline sampler over 256 times: worst dt %.5f gu, dq %.2e, ds %.2e\n", wt,
                  wq, ws);
      // Source-data sanity: the decoded control-point extremes. A wild range
      // here means the source spline decode is broken, not the transcode.
      float cp_min = 0, cp_max = 0;
      for (const auto& block : anim->blocks) {
        for (const auto& track : block.tracks) {
          for (float v : track.position.control_points) {
            cp_min = std::min(cp_min, v);
            cp_max = std::max(cp_max, v);
          }
        }
      }
      std::printf("  source position control points span [%.3f, %.3f]\n", cp_min, cp_max);
      for (size_t b = 0; b < anim->blocks.size(); ++b) {
        const auto& tracks = anim->blocks[b].tracks;
        for (size_t t = 0; t < tracks.size(); ++t) {
          const auto& ch = tracks[t].position;
          float lo = 0, hi = 0;
          for (float v : ch.control_points) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
          }
          if (hi > 1e6f || lo < -1e6f) {
            std::printf("  WILD block %zu track %zu: degree %u, %zu cps, span [%.3g, %.3g]\n", b,
                        t, ch.degree, ch.control_points.size() / 3, lo, hi);
          }
        }
      }
      auto blob = rec::bethesda::TranscodeToKinema(*anim, nullptr, nullptr);
      auto clip = kinema::Clip::FromBlob(blob.data(), blob.size());
      kinema::PoseArena arena(anim->num_tracks, 1);
      kinema::PoseView pose = arena.Acquire();
      std::vector<rec::bethesda::HkxTrackPose> reference;
      constexpr int kIters = 20000;
      volatile float sink = 0;
      auto t0 = std::chrono::steady_clock::now();
      for (int it = 0; it < kIters; ++it) {
        clip->Sample(anim->duration * static_cast<float>(it % 100) / 100.0f, pose);
        sink += pose.rotation[0].w;
      }
      auto t1 = std::chrono::steady_clock::now();
      for (int it = 0; it < kIters; ++it) {
        rec::bethesda::SampleAnimation(*anim, anim->duration * static_cast<float>(it % 100) / 100.0f,
                                       &reference);
        sink += reference[0].rotation[3];
      }
      auto t2 = std::chrono::steady_clock::now();
      auto ns = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
      };
      std::printf("  sample cost: kinema %lld ns/pose, spline sampler %lld ns/pose (%.1fx)\n",
                  static_cast<long long>(ns(t0, t1) / kIters),
                  static_cast<long long>(ns(t1, t2) / kIters),
                  static_cast<double>(ns(t1, t2)) / static_cast<double>(std::max<long long>(ns(t0, t1), 1)));
    } else if (args[i] == "--animnames") {
      auto names = rec::bethesda::DecodeAnimationNames(*hkx);
      std::printf("%zu animation names:\n", names.size());
      for (size_t n = 0; n < names.size(); ++n) {
        std::printf("  [%4zu] %s\n", n, names[n].c_str());
      }
    } else if (args[i] == "--hex" && i + 1 < args.size()) {
      rec::u64 offset = std::strtoull(args[++i].c_str(), nullptr, 0);
      rec::u64 count = 128;
      if (i + 1 < args.size() && args[i + 1][0] != '-') {
        count = std::strtoull(args[++i].c_str(), nullptr, 0);
      }
      PrintHex(*hkx, offset, count);
    } else if (args[i] == "--skeleton") {
      auto physics = rec::bethesda::DecodePhysics(*hkx);
      for (const auto& skeleton : physics.skeletons) {
        std::printf("skeleton '%s': %zu bones\n", skeleton.name.c_str(), skeleton.bones.size());
        for (size_t b = 0; b < skeleton.bones.size(); ++b) {
          const auto& bone = skeleton.bones[b];
          std::printf("  [%3zu] parent %3d  t(%7.2f %7.2f %7.2f)  %s\n", b, bone.parent,
                      bone.translation.x, bone.translation.y, bone.translation.z,
                      bone.name.c_str());
        }
      }
    } else if (args[i] == "--physics") {
      auto physics = rec::bethesda::DecodePhysics(*hkx);
      std::printf("%zu bodies, %zu constraints%s\n", physics.bodies.size(),
                  physics.constraints.size(), physics.ragdoll ? ", ragdoll" : "");
      auto shape_desc = [](const rec::bethesda::HkxShape& s) {
        char buf[160];
        switch (s.kind) {
          case rec::bethesda::HkxShape::Kind::kCapsule:
            std::snprintf(buf, sizeof(buf), "capsule r=%.2f a(%.1f %.1f %.1f) b(%.1f %.1f %.1f)",
                          s.radius, s.a.x, s.a.y, s.a.z, s.b.x, s.b.y, s.b.z);
            break;
          case rec::bethesda::HkxShape::Kind::kSphere:
            std::snprintf(buf, sizeof(buf), "sphere r=%.2f", s.radius);
            break;
          case rec::bethesda::HkxShape::Kind::kBox:
            std::snprintf(buf, sizeof(buf), "box (%.2f %.2f %.2f)", s.half_extents.x,
                          s.half_extents.y, s.half_extents.z);
            break;
          case rec::bethesda::HkxShape::Kind::kConvexVertices:
            std::snprintf(buf, sizeof(buf), "convex %zu verts", s.vertices.size());
            break;
          case rec::bethesda::HkxShape::Kind::kList:
            std::snprintf(buf, sizeof(buf), "list of %zu", s.children.size());
            break;
          case rec::bethesda::HkxShape::Kind::kTransform:
            std::snprintf(buf, sizeof(buf), "transform of %zu", s.children.size());
            break;
          default:
            std::snprintf(buf, sizeof(buf), "unknown(%s)", s.class_name.c_str());
        }
        return std::string(buf);
      };
      for (size_t b = 0; b < physics.bodies.size(); ++b) {
        const auto& body = physics.bodies[b];
        std::printf("  body[%2zu] '%s' motion %u mass %.2f fric %.2f pos(%.1f %.1f %.1f) %s\n",
                    b, body.name.c_str(), body.motion_type, body.mass, body.friction,
                    body.position.x, body.position.y, body.position.z,
                    shape_desc(body.shape).c_str());
      }
      constexpr double kRad2Deg = 57.29577951;
      for (const auto& c : physics.constraints) {
        if (c.kind == rec::bethesda::HkxConstraint::Kind::kRagdoll) {
          std::printf(
              "  ragdoll '%s' %d<->%d twist[%.0f..%.0f] cone %.0f plane[%.0f..%.0f] deg\n",
              c.name.c_str(), c.body_a, c.body_b, c.twist_min * kRad2Deg,
              c.twist_max * kRad2Deg, c.cone_max * kRad2Deg, c.plane_min * kRad2Deg,
              c.plane_max * kRad2Deg);
        } else if (c.kind == rec::bethesda::HkxConstraint::Kind::kLimitedHinge) {
          std::printf("  hinge   '%s' %d<->%d angle[%.0f..%.0f] deg\n", c.name.c_str(),
                      c.body_a, c.body_b, c.hinge_min * kRad2Deg, c.hinge_max * kRad2Deg);
        } else {
          std::printf("  other   '%s' %d<->%d\n", c.name.c_str(), c.body_a, c.body_b);
        }
      }
    } else if (args[i] == "--anim") {
      // Decode + sample the spline-compressed animation. Optional arg: time.
      rec::f32 at_time = 0.0f;
      if (i + 1 < args.size() && args[i + 1][0] != '-') {
        at_time = std::strtof(args[++i].c_str(), nullptr);
      }
      auto anim = rec::bethesda::DecodeAnimation(*hkx);
      if (!anim) {
        std::fprintf(stderr, "no decodable spline-compressed animation\n");
        return 1;
      }
      std::printf("animation: %.2fs, %u tracks, %u frames, %zu blocks, skeleton '%s'%s%s\n",
                  anim->duration, anim->num_tracks, anim->num_frames, anim->blocks.size(),
                  anim->skeleton_name.c_str(),
                  anim->track_to_bone.empty() ? " (identity track map)" : "",
                  anim->additive ? " ADDITIVE" : "");
      std::vector<rec::bethesda::HkxTrackPose> pose;
      rec::bethesda::SampleAnimation(*anim, at_time, &pose);
      int bad_quats = 0;
      for (const auto& p : pose) {
        rec::f32 len = std::sqrt(p.rotation[0] * p.rotation[0] + p.rotation[1] * p.rotation[1] +
                                 p.rotation[2] * p.rotation[2] + p.rotation[3] * p.rotation[3]);
        if (std::fabs(len - 1.0f) > 0.02f) ++bad_quats;
      }
      std::printf("sampled t=%.2f: %d non-unit quats\n", at_time, bad_quats);
      for (size_t t = 0; t < pose.size() && t < 8; ++t) {
        std::printf("  track[%2zu] t(%7.2f %7.2f %7.2f) q(%.3f %.3f %.3f %.3f) s %.2f\n", t,
                    pose[t].translation.x, pose[t].translation.y, pose[t].translation.z,
                    pose[t].rotation[0], pose[t].rotation[1], pose[t].rotation[2],
                    pose[t].rotation[3], pose[t].scale);
      }
    } else if (args[i] == "--ragdoll") {
      // Full ragdoll drop test: spawn the bodies in bind pose (rotated from
      // the data's Z-up into the engine's Y-up), joint them per the decoded
      // constraints, drop onto a floor for 4 simulated seconds, then check
      // the doll stayed in one piece: no NaNs, every joint's two world-space
      // pivots (computed independently through body A and body B) still
      // coincide, nothing fell through the floor.
      auto physics = rec::bethesda::DecodePhysics(*hkx);
      rec::physics::PhysicsWorld world;
      if (!world.Initialize()) {
        std::fprintf(stderr, "jolt world init failed (stub linked?)\n");
        return 1;
      }
      constexpr rec::f32 kScale = 0.01428f;  // game units -> meters
      const rec::Quat kZupToYup{-0.70710678f, 0.0f, 0.0f, 0.70710678f};  // -90 deg about X
      world.AddStaticBox({0.0f, -0.5f, 0.0f}, {50.0f, 0.5f, 50.0f});

      std::vector<rec::physics::BodyId> ids(physics.bodies.size(), 0);
      rec::i32 filter = world.CreateBodyFilterGroup(static_cast<rec::u32>(physics.bodies.size()));
      int spawned = 0;
      for (size_t b = 0; b < physics.bodies.size(); ++b) {
        const auto& body = physics.bodies[b];
        if (body.mass <= 0.0f) continue;  // keyframed helpers (CharacterBumper)
        rec::physics::ShapeDesc desc = rec::bethesda::ToShapeDesc(body.shape);
        rec::Vec3 pos = rec::Rotate(kZupToYup, body.position * kScale);
        pos.y += 1.0f;  // drop height
        rec::Quat body_rot{body.rotation[0], body.rotation[1], body.rotation[2],
                           body.rotation[3]};
        rec::Quat rot = kZupToYup * body_rot;
        rec::f32 rot4[4] = {rot.x, rot.y, rot.z, rot.w};
        ids[b] = world.AddDynamicShape(desc, pos, rot4, kScale, body.mass, body.friction,
                                       body.restitution, filter, static_cast<rec::u32>(b));
        if (ids[b] != 0) ++spawned;
      }
      // Skyrim's authored per-shape collision filter info governs ragdoll
      // self-collision; until that is decoded, disable it wholesale (folded
      // dragon wings overlap torso parts they are not jointed to).
      for (size_t x = 0; x < physics.bodies.size(); ++x) {
        for (size_t y = x + 1; y < physics.bodies.size(); ++y) {
          world.DisableFilterPair(filter, static_cast<rec::u32>(x), static_cast<rec::u32>(y));
        }
      }
      int joints = 0;
      for (const auto& c : physics.constraints) {
        if (c.body_a < 0 || c.body_b < 0) continue;
        if (ids[c.body_a] == 0 || ids[c.body_b] == 0) continue;
        bool ok = false;
        if (c.kind == rec::bethesda::HkxConstraint::Kind::kRagdoll) {
          ok = world.AddSwingTwistJoint(ids[c.body_a], ids[c.body_b], c.frame_a, c.frame_b,
                                        kScale, c.twist_min, c.twist_max, c.cone_max,
                                        c.plane_min, c.plane_max);
        } else if (c.kind == rec::bethesda::HkxConstraint::Kind::kLimitedHinge) {
          ok = world.AddHingeJoint(ids[c.body_a], ids[c.body_b], c.frame_a, c.frame_b, kScale,
                                   c.hinge_min, c.hinge_max);
        }
        if (ok) ++joints;
      }
      for (int step = 0; step < 240; ++step) world.Update(1.0f / 60.0f);

      auto world_pivot = [&](int body, const rec::f32 frame[12]) {
        rec::Vec3 pos;
        rec::f32 rot4[4];
        world.GetBodyTransform(ids[body], &pos, rot4);
        rec::Quat rot{rot4[0], rot4[1], rot4[2], rot4[3]};
        rec::Vec3 local{frame[3] * kScale, frame[7] * kScale, frame[11] * kScale};
        return pos + rec::Rotate(rot, local);
      };
      rec::f32 max_separation = 0.0f, min_y = 1e9f, max_y = -1e9f;
      bool nan = false;
      for (size_t b = 0; b < physics.bodies.size(); ++b) {
        if (ids[b] == 0) continue;
        rec::Vec3 pos;
        rec::f32 rot4[4];
        world.GetBodyTransform(ids[b], &pos, rot4);
        if (pos.x != pos.x || pos.y != pos.y || pos.z != pos.z) nan = true;
        min_y = std::min(min_y, pos.y);
        max_y = std::max(max_y, pos.y);
      }
      for (const auto& c : physics.constraints) {
        if (c.kind == rec::bethesda::HkxConstraint::Kind::kOther) continue;
        if (c.body_a < 0 || c.body_b < 0 || ids[c.body_a] == 0 || ids[c.body_b] == 0) continue;
        rec::Vec3 pa = world_pivot(c.body_a, c.frame_a);
        rec::Vec3 pb = world_pivot(c.body_b, c.frame_b);
        rec::Vec3 d = pa - pb;
        rec::f32 sep = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        max_separation = std::max(max_separation, sep);
      }
      bool pass = !nan && max_separation < 0.05f && min_y > -0.1f;
      std::printf(
          "ragdoll: %d bodies, %d joints, 240 steps; max joint separation %.1f mm, "
          "rest height %.2f..%.2f m, nan %s -> %s\n",
          spawned, joints, max_separation * 1000.0f, min_y, max_y, nan ? "YES" : "no",
          pass ? "PASS" : "FAIL");
      if (!pass) return 1;
    } else if (args[i] == "--jolt") {
      // Smoke test: lower every decoded body's shape into a live Jolt world
      // (game-unit scale) and report what stuck.
      auto physics = rec::bethesda::DecodePhysics(*hkx);
      rec::physics::PhysicsWorld world;
      if (!world.Initialize()) {
        std::fprintf(stderr, "jolt world init failed\n");
        return 1;
      }
      constexpr rec::f32 kUnitsToMeters = 0.01428f;
      int ok = 0, failed = 0;
      for (const auto& body : physics.bodies) {
        rec::physics::ShapeDesc desc = rec::bethesda::ToShapeDesc(body.shape);
        rec::physics::BodyId id =
            body.mass > 0.0f
                ? world.AddDynamicShape(desc, body.position * kUnitsToMeters, body.rotation,
                                        kUnitsToMeters, body.mass, body.friction,
                                        body.restitution)
                : world.AddStaticShape(desc, body.position * kUnitsToMeters, body.rotation,
                                       kUnitsToMeters);
        if (id != 0) {
          ++ok;
        } else {
          ++failed;
          std::printf("  FAILED '%s' (%s)\n", body.name.c_str(),
                      body.shape.class_name.c_str());
        }
      }
      for (int step = 0; step < 60; ++step) world.Update(1.0f / 60.0f);
      std::printf("jolt: %d bodies created, %d failed, 60 steps simulated\n", ok, failed);
    } else if (args[i] == "--sections") {
      // Header line above already covers the summary.
    } else {
      std::fprintf(stderr, "unknown mode %s\n", args[i].c_str());
    }
  }
  if (!any_mode) PrintClasses(*hkx);
  return 0;
}

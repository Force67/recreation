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
#include "physics/physics_world.h"

namespace {

using rec::bethesda::HkxFile;

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
      if (entry.path().extension() == ".bsa") {
        if (auto provider = rec::bethesda::OpenArchive(entry.path().string())) {
          vfs.Mount(std::move(provider));
        }
      }
    }
    std::printf("mounted %zu archives\n", vfs.mount_count());
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
      std::ofstream out(args[++i], std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
      std::printf("wrote %s\n", args[i].c_str());
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

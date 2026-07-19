#include "feature_gym.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "anim/anim_graph.h"
#include "anim/foot_placement.h"
#include "anim/morph.h"
#include "anim/pose.h"
#include "anim/rig_player.h"
#include "asset/asset_id.h"
#include "asset/material.h"
#include "asset/primitives.h"
#include "asset/texture.h"
#include "audio/audio_clip.h"
#include "audio/mixer.h"
#include "core/log.h"
#include "core/math.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#include "../engine_context.h"
#include "physics/shape_desc.h"
#include "physics/water_waves.h"
#include "render/geometry/hair_groom.h"
#include "render/geometry/imposters.h"
#include "render/pipeline/mesh_pipeline.h"
#include "scene/camera.h"
#include "scene/camera_rig.h"
#include "../showcase_camera.h"
#include "world/components.h"

#if defined(__ANDROID__)
#include <android/asset_manager.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#if RECREATION_HAS_NET
#include "net/bubble.h"
#include "net/bubble_debug.h"
#endif

#if defined(RECREATION_HAS_IMGUI)
#include <imgui.h>
#endif

#ifndef RECREATION_FEATURE_GYM_ASSET_DIR
#define RECREATION_FEATURE_GYM_ASSET_DIR "feature_gym-assets"
#endif

namespace rx {
namespace {

#if defined(__ANDROID__)
AAssetManager* g_feature_gym_asset_manager = nullptr;
#endif

constexpr f32 kPi = 3.14159265358979323846f;
constexpr u32 kTextureSize = 128;
constexpr f32 kVirtualGeometrySize = 9.0f;

enum class Area : u8 {
  kOverview,
  kMaterials,
  kLighting,
  kGeometry,
  kAtmosphere,
  kWater,
  kEffects,
  kPhysics,
  kAnimation,
  kPost,
  kInterior,
  kPathTrace,
};

enum class TourMode : u8 {
  kOverview,
  kMaterials,
  kSplitPbrPerSubmesh,
  kTerrainSplatV2,
  kLightingRaster,
  kLightingRaytraced,
  kLightingRcgi,
  kGeometryScalability,
  kStreamedInstanceLifecycle,
  kProjectedVirtualGeometryAlbedo,
  kWeatherRain,
  kWeatherSnowAurora,
  kWaterOcean,
  kGerstnerShorelineBuoyancy,
  kEffects,
  kJoltProceduralStrandGroom,
  kPhysics,
  kTransportFreeNetworkBubbles,
  kAnimation,
  kEcsCameraStackRig,
  kPostTaa,
  kPostMsaa,
  kPostFsr3,
  kPostDlss,
  kPostXess,
  kPostTonemapGrade,
  kInteriorFog,
  kPathTraceReconstruction,
};

struct AreaInfo {
  const char* name;
  Vec3 center;
  u32 color;
};

constexpr AreaInfo kAreas[] = {
    {"MATERIALS", {-30, 0, 18}, 0xe9a84bff},
    {"LIGHTING", {0, 0, 18}, 0xff695dff},
    {"GEOMETRY", {30, 0, 18}, 0x56bdefff},
    {"ATMOSPHERE", {-30, 0, -12}, 0x8e7de8ff},
    {"WATER", {0, 0, -12}, 0x42d5dfff},
    {"EFFECTS", {30, 0, -12}, 0xff66b3ff},
    {"PHYSICS", {-30, 0, -42}, 0x7ed36fff},
    {"ANIMATION", {0, 0, -42}, 0xffca58ff},
    {"POST + DISPLAY", {30, 0, -42}, 0xeaeef5ff},
};

const AreaInfo& Info(Area area) {
  switch (area) {
    case Area::kMaterials: return kAreas[0];
    case Area::kLighting: return kAreas[1];
    case Area::kGeometry: return kAreas[2];
    case Area::kAtmosphere: return kAreas[3];
    case Area::kWater: return kAreas[4];
    case Area::kEffects: return kAreas[5];
    case Area::kPhysics: return kAreas[6];
    case Area::kAnimation: return kAreas[7];
    case Area::kPost:
    case Area::kInterior:
    case Area::kPathTrace: return kAreas[8];
    case Area::kOverview: break;
  }
  static constexpr AreaInfo overview{"OVERVIEW", {0, 0, -12}, 0xffffffff};
  return overview;
}

struct GymMotion {
  Vec3 origin{};
  Vec3 axis{0, 1, 0};
  f32 speed = 1;
  f32 amplitude = 0;
  f32 phase = 0;
};

struct Particle {
  Vec3 position{};
  Vec3 velocity{};
  Vec3 color{1, 1, 1};
  f32 life = 0;
  f32 max_life = 1;
  f32 size = 0.1f;
};

#if RECREATION_HAS_NET
struct BubbleAgent {
  f32 time = 0;
  f32 rate_x = 0.3f;
  f32 rate_z = 0.2f;
  f32 extent = 5.0f;
  Vec3 center{};
};
#endif

asset::Mesh WithMaterial(asset::Mesh mesh, asset::AssetId material) {
  for (asset::MeshLod& lod : mesh.lods) {
    if (lod.submeshes.empty())
      lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    else
      for (asset::Submesh& submesh : lod.submeshes) submesh.material = material;
  }
  return mesh;
}

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
  std::array<wchar_t, 4096> path{};
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (size > 0 && size < path.size())
    return std::filesystem::path(std::wstring_view(path.data(), size)).parent_path();
#elif defined(__APPLE__)
  std::array<char, 4096> path{};
  u32 size = static_cast<u32>(path.size());
  if (_NSGetExecutablePath(path.data(), &size) == 0)
    return std::filesystem::weakly_canonical(path.data()).parent_path();
#else
  std::array<char, 4096> path{};
  const ssize_t size = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size > 0) return std::filesystem::path(std::string_view(path.data(), size)).parent_path();
#endif
  return std::filesystem::current_path();
}

std::filesystem::path FeatureAssetPath(const char* file) {
  if (const char* root = std::getenv("RECREATION_FEATURE_GYM_ASSET_DIR"))
    return std::filesystem::path(root) / file;
  return std::filesystem::path(RECREATION_FEATURE_GYM_ASSET_DIR) / file;
}

std::vector<u8> ReadBytes(const std::filesystem::path& requested_path) {
#if defined(__ANDROID__)
  if (g_feature_gym_asset_manager) {
    const std::string name = requested_path.filename().string();
    AAsset* asset =
        AAssetManager_open(g_feature_gym_asset_manager, name.c_str(), AASSET_MODE_STREAMING);
    if (asset) {
      const off64_t size = AAsset_getLength64(asset);
      std::vector<u8> bytes(size > 0 ? static_cast<size_t>(size) : 0);
      const int read = bytes.empty() ? 0 : AAsset_read(asset, bytes.data(), bytes.size());
      AAsset_close(asset);
      if (read == static_cast<int>(bytes.size())) return bytes;
    }
  }
#endif

  std::filesystem::path path = requested_path;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input && path.is_relative()) {
    path = ExecutableDirectory() / path;
    input = std::ifstream(path, std::ios::binary | std::ios::ate);
  }
  if (!input) return {};
  const std::streamsize size = input.tellg();
  if (size <= 0) return {};
  std::vector<u8> bytes(static_cast<size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) return {};
  return bytes;
}

asset::Texture LoadRawTexture(const char* file, const char* id, bool srgb) {
  asset::Texture texture;
  texture.id = asset::MakeAssetId(id);
  texture.format = asset::TextureFormat::kRgba8;
  texture.width = kTextureSize;
  texture.height = kTextureSize;
  texture.is_srgb = srgb;
  const std::filesystem::path path = FeatureAssetPath(file);
  std::vector<u8> bytes = ReadBytes(path);
  const size_t expected = static_cast<size_t>(kTextureSize) * kTextureSize * 4;
  if (bytes.size() != expected) {
    RX_WARN("feature gym: {} has {} bytes, expected {} (run generate_assets.py)", path.string(),
            bytes.size(), expected);
    return texture;
  }
  texture.data.assign(bytes.begin(), bytes.end());
  return texture;
}

std::vector<u8> BuildRgbaMipChain(const asset::Texture& texture) {
  const size_t expected = static_cast<size_t>(texture.width) * texture.height * 4;
  if (texture.width != texture.height || texture.data.size() != expected || texture.width == 0)
    return {};

  u32 extent = texture.width;
  std::vector<u8> level(texture.data.begin(), texture.data.end());
  std::vector<u8> mips;
  mips.reserve(expected * 4 / 3);
  while (true) {
    mips.insert(mips.end(), level.begin(), level.end());
    if (extent == 1) break;
    const u32 next_extent = std::max(1u, extent / 2);
    std::vector<u8> next(static_cast<size_t>(next_extent) * next_extent * 4);
    for (u32 y = 0; y < next_extent; ++y) {
      for (u32 x = 0; x < next_extent; ++x) {
        for (u32 channel = 0; channel < 4; ++channel) {
          u32 sum = 0;
          for (u32 oy = 0; oy < 2; ++oy) {
            for (u32 ox = 0; ox < 2; ++ox) {
              const u32 sx = std::min(extent - 1, x * 2 + ox);
              const u32 sy = std::min(extent - 1, y * 2 + oy);
              sum += level[(static_cast<size_t>(sy) * extent + sx) * 4 + channel];
            }
          }
          next[(static_cast<size_t>(y) * next_extent + x) * 4 + channel] =
              static_cast<u8>((sum + 2) / 4);
        }
      }
    }
    level = std::move(next);
    extent = next_extent;
  }
  return mips;
}

physics::PhysicsWorld::StrandGroomDesc ToStrandGroomDesc(const render::GroomData& data) {
  physics::PhysicsWorld::StrandGroomDesc desc;
  desc.points = data.points.data();
  desc.strand_count = data.guide_count;
  desc.points_per_strand = render::kGroomPointsPerStrand;
  desc.pins = data.pins.data();
  desc.pin_count = static_cast<u32>(data.pins.size() / 2);
  desc.binds = data.binds.data();
  desc.bind_count = static_cast<u32>(data.binds.size() / 4);
  desc.stretch_compliance = data.sim.stretch_compliance;
  desc.bend_compliance = data.sim.bend_compliance;
  desc.bind_compliance = data.sim.bind_compliance;
  desc.damping = data.sim.damping;
  desc.gravity_factor = data.sim.gravity_factor;
  desc.node_mass = data.sim.node_mass;
  desc.node_radius = data.sim.node_radius;
  desc.max_stretch = data.sim.max_stretch;
  desc.iterations = data.sim.iterations;
  return desc;
}

Area AreaForMode(TourMode mode) {
  switch (mode) {
    case TourMode::kOverview: return Area::kOverview;
    case TourMode::kMaterials:
    case TourMode::kSplitPbrPerSubmesh:
    case TourMode::kTerrainSplatV2: return Area::kMaterials;
    case TourMode::kLightingRaster:
    case TourMode::kLightingRaytraced:
    case TourMode::kLightingRcgi: return Area::kLighting;
    case TourMode::kGeometryScalability:
    case TourMode::kStreamedInstanceLifecycle:
    case TourMode::kProjectedVirtualGeometryAlbedo: return Area::kGeometry;
    case TourMode::kWeatherRain:
    case TourMode::kWeatherSnowAurora: return Area::kAtmosphere;
    case TourMode::kWaterOcean:
    case TourMode::kGerstnerShorelineBuoyancy: return Area::kWater;
    case TourMode::kEffects:
    case TourMode::kJoltProceduralStrandGroom:
    case TourMode::kTransportFreeNetworkBubbles: return Area::kEffects;
    case TourMode::kPhysics: return Area::kPhysics;
    case TourMode::kAnimation: return Area::kAnimation;
    case TourMode::kEcsCameraStackRig:
    case TourMode::kPostTaa:
    case TourMode::kPostMsaa:
    case TourMode::kPostFsr3:
    case TourMode::kPostDlss:
    case TourMode::kPostXess:
    case TourMode::kPostTonemapGrade: return Area::kPost;
    case TourMode::kInteriorFog: return Area::kInterior;
    case TourMode::kPathTraceReconstruction: return Area::kPathTrace;
  }
  return Area::kOverview;
}

asset::Mesh MakeWaterGrid(Vec3 center, f32 half_extent, u32 cells, asset::AssetId id,
                          asset::AssetId material) {
  asset::Mesh mesh;
  mesh.id = id;
  mesh.lods.emplace_back();
  asset::MeshLod& lod = mesh.lods[0];
  lod.vertices.reserve(static_cast<size_t>(cells + 1) * (cells + 1));
  for (u32 z = 0; z <= cells; ++z) {
    for (u32 x = 0; x <= cells; ++x) {
      const f32 u = static_cast<f32>(x) / cells;
      const f32 v = static_cast<f32>(z) / cells;
      asset::Vertex vertex{};
      vertex.position[0] = center.x + (u * 2.0f - 1.0f) * half_extent;
      vertex.position[1] = center.y;
      vertex.position[2] = center.z + (v * 2.0f - 1.0f) * half_extent;
      vertex.normal[1] = 1;
      vertex.tangent[0] = 1;
      vertex.tangent[3] = 1;
      vertex.uv[0] = u * 8.0f;
      vertex.uv[1] = v * 8.0f;
      lod.vertices.push_back(vertex);
    }
  }
  for (u32 z = 0; z < cells; ++z) {
    for (u32 x = 0; x < cells; ++x) {
      const u32 a = z * (cells + 1) + x;
      const u32 b = a + 1;
      const u32 c = a + cells + 1;
      const u32 d = c + 1;
      for (u32 index : {a, b, c, b, d, c}) lod.indices.push_back(index);
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
  mesh.bounds_center[0] = center.x;
  mesh.bounds_center[1] = center.y;
  mesh.bounds_center[2] = center.z;
  mesh.bounds_radius = half_extent * 1.5f;
  return mesh;
}

asset::Mesh MakeBanner(asset::AssetId id, asset::AssetId material) {
  asset::Mesh mesh;
  mesh.id = id;
  mesh.lods.emplace_back();
  asset::MeshLod& lod = mesh.lods[0];
  constexpr int kX = 8;
  constexpr int kY = 12;
  for (int y = 0; y <= kY; ++y) {
    for (int x = 0; x <= kX; ++x) {
      const f32 u = static_cast<f32>(x) / kX;
      const f32 v = static_cast<f32>(y) / kY;
      asset::Vertex vertex{};
      vertex.position[0] = (u - 0.5f) * 2.2f;
      vertex.position[1] = -v * 3.0f;
      vertex.normal[2] = 1;
      vertex.tangent[0] = 1;
      vertex.tangent[3] = 1;
      vertex.uv[0] = u;
      vertex.uv[1] = v;
      lod.vertices.push_back(vertex);
    }
  }
  for (int y = 0; y < kY; ++y) {
    for (int x = 0; x < kX; ++x) {
      const u32 a = y * (kX + 1) + x;
      const u32 b = a + 1;
      const u32 c = a + kX + 1;
      const u32 d = c + 1;
      for (u32 index : {a, c, b, b, c, d}) lod.indices.push_back(index);
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
  mesh.bounds_center[1] = -1.5f;
  mesh.bounds_radius = 2.0f;
  return mesh;
}

asset::Mesh MakeTree(asset::AssetId id, asset::AssetId material) {
  asset::Mesh tree;
  tree.id = id;
  tree.lods.emplace_back();
  asset::MeshLod& lod = tree.lods[0];
  auto triangle = [&](Vec3 a, Vec3 b, Vec3 c, u32 color) {
    const Vec3 normal = Normalize(Cross(b - a, c - a));
    const u32 first = static_cast<u32>(lod.vertices.size());
    for (const Vec3& p : {a, b, c}) {
      asset::Vertex v{};
      v.position[0] = p.x;
      v.position[1] = p.y;
      v.position[2] = p.z;
      v.normal[0] = normal.x;
      v.normal[1] = normal.y;
      v.normal[2] = normal.z;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.color = color;
      lod.vertices.push_back(v);
    }
    lod.indices.push_back(first);
    lod.indices.push_back(first + 1);
    lod.indices.push_back(first + 2);
  };
  for (int tier = 0; tier < 3; ++tier) {
    const f32 base = 0.5f + tier * 0.75f;
    const f32 tip = base + 1.5f;
    const f32 radius = 1.15f - tier * 0.25f;
    for (int side = 0; side < 10; ++side) {
      const f32 a0 = side * 2.0f * kPi / 10.0f;
      const f32 a1 = (side + 1) * 2.0f * kPi / 10.0f;
      triangle({std::cos(a0) * radius, base, std::sin(a0) * radius}, {0, tip, 0},
               {std::cos(a1) * radius, base, std::sin(a1) * radius},
               side & 1 ? 0xff245a27 : 0xff377a31);
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
  tree.bounds_center[1] = 1.6f;
  tree.bounds_radius = 2.5f;
  return tree;
}

render::Decal MakeDecal(Vec3 position, Vec3 normal, Vec3 up, f32 width, f32 height) {
  render::Decal decal;
  const Vec3 n = Normalize(normal);
  const Vec3 tangent = Normalize(Cross(up, n));
  const Vec3 bitangent = Cross(n, tangent);
  auto row = [&](Vec3 axis, f32 extent, f32* out) {
    out[0] = axis.x / extent;
    out[1] = axis.y / extent;
    out[2] = axis.z / extent;
    out[3] = -Dot(axis, position) / extent;
  };
  row(tangent, width, decal.row0);
  row(bitangent, height, decal.row1);
  row(n, 0.4f, decal.row2);
  decal.params2[0] = 0.8f;
  decal.params2[1] = 0.55f;
  decal.params2[2] = 0.2f;
  return decal;
}

constexpr u64 NameHash(const char* text) {
  u64 hash = 14695981039346656037ull;
  while (*text) {
    hash ^= static_cast<u8>(*text++);
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

#if defined(__ANDROID__)
void SetFeatureGymAssetManager(AAssetManager* manager) { g_feature_gym_asset_manager = manager; }
#endif

struct FeatureGym::Impl {
  explicit Impl(EngineContext& context)
      : ctx(context),
        world(*context.world),
        scheduler(*context.scheduler),
        renderer(*context.renderer),
         physics(*context.physics),
         headless(context.config->headless) {}
  ~Impl();

  void Create();
  void CreateBase();
  void CreateMaterials();
  void CreateLighting();
  void CreateGeometry();
  void CreateAtmosphere();
  void CreateWater();
  void CreateEffects();
  void CreateNetworkBubbles();
  void CreatePhysics();
  void CreateCloth();
  void CreateAnimation();
  void CreatePost();
  void CreateCameraExhibit();
  void StartAudio();
  void AddSimulation();
  void Emit(f32 dt, render::FrameView& view);
  void UpdateInstanceExhibit();
  void EmitStrandGroom();
  void EmitNetworkBubbles(render::FrameView& view);
  void EmitCameraExhibit(f32 dt, render::FrameView& view);
  void UpdateParticles(f32 dt, render::FrameView& view);
  void EmitCloth(render::FrameView& view);
  void EmitAnimation(render::FrameView& view);
  void EmitVehicles(render::FrameView& view);
  void DrawPanel();
  bool BuildTour(ShowcaseCamera& camera);
  void SetTourTime(f32 seconds);
  void ApplyArea(Area area);
  void ApplyTourMode(TourMode mode);

  asset::AssetId AddMaterial(const std::string& name, Vec3 color, f32 roughness = 0.7f,
                             f32 metallic = 0.0f);
  asset::AssetId UploadMesh(asset::Mesh mesh, asset::AssetId material, bool trace = false);
  ecs::Entity Spawn(asset::AssetId mesh, Vec3 position, f32 scale = 1.0f,
                     Quat rotation = {});
  ecs::Entity SpawnBox(const std::string& name, Vec3 half_extent, Vec3 position,
                       asset::AssetId material, bool collider = false, bool trace = false);

  EngineContext& ctx;
  ecs::World& world;
  ecs::Scheduler& scheduler;
  render::Renderer& renderer;
  physics::PhysicsWorld& physics;
  bool headless = false;
  bool created = false;

  render::RenderSettings baseline{};
  Area active = Area::kOverview;
  TourMode active_mode = TourMode::kOverview;
  f32 active_mode_elapsed = 0;
  struct Activation {
    f32 time = 0;
    TourMode mode = TourMode::kOverview;
  };
  std::vector<Activation> activations;
  size_t applied_activation = std::numeric_limits<size_t>::max();

  asset::AssetId checker{};
  asset::AssetId albedo{};
  asset::AssetId normal{};
  asset::AssetId object_normal{};
  asset::AssetId orm{};
  asset::AssetId roughness_map{};
  asset::AssetId metallic_map{};
  asset::AssetId ao_map{};
  asset::AssetId height{};
  asset::AssetId alpha{};
  asset::AssetId emissive{};
  asset::AssetId weights{};
  asset::AssetId weights_b{};
  asset::AssetId decal{};
  asset::AssetId decal_normal{};
  asset::AssetId neutral_material{};
  std::vector<u8> virtual_geometry_albedo;

  base::Vector<render::PointLight> lights;
  base::Vector<render::Decal> decals;
  base::Vector<render::WboitInstance> oit;
  base::Vector<render::GaussianInstance> gaussians;
  base::Vector<render::DebugLine> lines;
  base::Vector<Particle> particles;
  u32 particle_seed = 0x6d2b79f5u;
  f32 particle_spawn = 0;
  Vec3 particle_emitter{33, 0.5f, -12};

  u64 morph_mesh = 0;
  Mat4 morph_transform = Mat4::Identity();
  Mat4 morph_previous = Mat4::Identity();
  f32 render_time = 0;

  render::InstanceGroupHandle prop_group{};
  u64 prop_mesh = 0;
  base::Vector<Mat4> prop_transforms;
  base::Vector<Mat4> prop_updated_transforms;
  int prop_lifecycle_phase = 0;

  u32 strand_groom = 0;
  physics::StrandGroomId strand_sim = 0;
  Mat4 strand_transform = Mat4::Identity();
  Vec3 strand_center{};
  base::Vector<f32> strand_positions;

#if RECREATION_HAS_NET
  net::InterestMap bubble_map;
  std::unique_ptr<net::BubbleVisualizer> bubble_viz;
  u64 bubble_tick = 0;
#endif

  ecs::Entity camera_base_mode{};
  ecs::Entity camera_rig_mode{};
  ecs::Entity camera_output{};
  scene::CameraActivation camera_activation{};

  physics::ClothId cloth_id = 0;
  u64 cloth_mesh = 0;
  u32 cloth_width = 0;
  base::Vector<u32> cloth_indices;
  base::Vector<Vec3> cloth_positions;
  base::Vector<Vec3> cloth_normals;
  base::Vector<render::DebugLine> cloth_lines;

  asset::Skeleton skeleton;
  asset::SkinBinding skin;
  anim::AnimGraph graph;
  anim::RigPlayer rig;
  anim::FootPlacement feet;
  anim::SkeletonPose pose;
  base::Vector<i32> remap;
  base::Vector<Mat4> bone_model;
  base::Vector<Mat4> palette;
  u64 biped_mesh = 0;
  Vec3 biped_position{0, 0, -44};
  Mat4 biped_previous = Mat4::Identity();
  bool biped_previous_valid = false;
  f32 biped_time = 0;

  physics::BodyId platform_body = 0;
  ecs::Entity platform_entity{};
  Vec3 platform_origin{-35, 0.45f, -38};
  physics::CharacterId character = 0;
  ecs::Entity character_entity{};
  f32 character_direction = 1;
  physics::VehicleId car = 0;
  physics::VehicleId bike = 0;
  u64 car_mesh = 0;
  u64 wheel_mesh = 0;
  u64 bike_mesh = 0;
  Mat4 car_previous = Mat4::Identity();
  Mat4 bike_previous = Mat4::Identity();
  std::array<Mat4, 4> car_wheel_previous{};
  std::array<Mat4, 2> bike_wheel_previous{};
  bool car_previous_valid = false;
  bool bike_previous_valid = false;
  std::array<bool, 4> car_wheel_previous_valid{};
  std::array<bool, 2> bike_wheel_previous_valid{};
  f32 physics_active_time = 0;
  f32 sim_time = 0;
  int jump_cycle = -1;

  base::Vector<physics::BodyId> water_bodies;
  base::Vector<Vec3> water_previous;
  f32 water_height = 0;
  u32 audio_voice = 0;
};

FeatureGym::Impl::~Impl() {
  if (prop_group) renderer.DestroyInstanceGroup(prop_group);
  if (strand_sim) physics.RemoveStrandGroom(strand_sim);
  if (strand_groom) renderer.DestroyHairGroom(strand_groom);
}

asset::AssetId FeatureGym::Impl::AddMaterial(const std::string& name, Vec3 color, f32 roughness,
                                              f32 metallic) {
  asset::Material material;
  material.id = asset::MakeAssetId("featuregym/material/" + name);
  material.base_color_factor[0] = color.x;
  material.base_color_factor[1] = color.y;
  material.base_color_factor[2] = color.z;
  material.roughness_factor = roughness;
  material.metallic_factor = metallic;
  if (!headless) renderer.UploadMaterial(material);
  return material.id;
}

asset::AssetId FeatureGym::Impl::UploadMesh(asset::Mesh mesh, asset::AssetId material,
                                             bool trace) {
  mesh = WithMaterial(std::move(mesh), material);
  if (ctx.config->renderer.software_gi && !trace) mesh.exclude_from_rt = true;
  const asset::AssetId id = mesh.id;
  if (!headless) renderer.UploadMesh(mesh);
  return id;
}

ecs::Entity FeatureGym::Impl::Spawn(asset::AssetId mesh, Vec3 position, f32 scale, Quat rotation) {
  ecs::Entity entity = world.Create();
  world.Add(entity, world::Transform{.position = {position.x, position.y, position.z},
                                     .rotation = {rotation.x, rotation.y, rotation.z, rotation.w},
                                     .scale = scale});
  world.Add(entity, world::Renderable{mesh});
  return entity;
}

ecs::Entity FeatureGym::Impl::SpawnBox(const std::string& name, Vec3 half_extent, Vec3 position,
                                        asset::AssetId material, bool collider, bool trace) {
  const asset::AssetId id =
      UploadMesh(asset::MakeBox(half_extent.x, half_extent.y, half_extent.z,
                                asset::MakeAssetId("featuregym/mesh/" + name)),
                 material, trace);
  ecs::Entity entity = Spawn(id, position);
  if (collider) physics.AddStaticBox(position, half_extent);
  return entity;
}

void FeatureGym::Impl::Create() {
  if (created) return;
  created = true;
  ctx.scene_owns_sun = true;

  const asset::Texture textures[] = {
      LoadRawTexture("checker.rgba", "featuregym/texture/checker", true),
      LoadRawTexture("albedo.rgba", "featuregym/texture/albedo", true),
      LoadRawTexture("normal.rgba", "featuregym/texture/normal", false),
      LoadRawTexture("object_normal.rgba", "featuregym/texture/object_normal", false),
      LoadRawTexture("orm.rgba", "featuregym/texture/orm", false),
      LoadRawTexture("roughness.rgba", "featuregym/texture/roughness", false),
      LoadRawTexture("metallic.rgba", "featuregym/texture/metallic", false),
      LoadRawTexture("ao.rgba", "featuregym/texture/ao", false),
      LoadRawTexture("height.rgba", "featuregym/texture/height", false),
      LoadRawTexture("alpha.rgba", "featuregym/texture/alpha", true),
      LoadRawTexture("emissive.rgba", "featuregym/texture/emissive", true),
      LoadRawTexture("weights.rgba", "featuregym/texture/weights", false),
      LoadRawTexture("weights_b.rgba", "featuregym/texture/weights_b", false),
      LoadRawTexture("decal.rgba", "featuregym/texture/decal", true),
      LoadRawTexture("decal_normal.rgba", "featuregym/texture/decal_normal", false),
  };
  checker = textures[0].id;
  albedo = textures[1].id;
  normal = textures[2].id;
  object_normal = textures[3].id;
  orm = textures[4].id;
  roughness_map = textures[5].id;
  metallic_map = textures[6].id;
  ao_map = textures[7].id;
  height = textures[8].id;
  alpha = textures[9].id;
  emissive = textures[10].id;
  weights = textures[11].id;
  weights_b = textures[12].id;
  decal = textures[13].id;
  decal_normal = textures[14].id;
  virtual_geometry_albedo = BuildRgbaMipChain(textures[1]);
  if (!headless) {
    for (const asset::Texture& texture : textures)
      if (!texture.data.empty()) renderer.UploadTexture(texture);
  }

  CreateBase();
  CreateMaterials();
  CreateLighting();
  CreateGeometry();
  CreateAtmosphere();
  CreateWater();
  CreateEffects();
  CreateNetworkBubbles();
  CreatePhysics();
  CreateAnimation();
  CreatePost();
  CreateCameraExhibit();
  AddSimulation();
  StartAudio();

  auto& settings = renderer.settings();
  settings.sun_direction = Normalize(Vec3{-0.45f, -0.78f, -0.42f});
  settings.sun_intensity = 3.6f;
  settings.sun_color = {1.0f, 0.96f, 0.90f};
  settings.ambient = 0.10f;
  settings.dof = false;
  settings.precipitation = 0;
  settings.lightning = 0;
  settings.aurora = false;
  settings.interior = false;
  settings.ibl = true;
  settings.rcgi = false;
  baseline = settings;

  ctx.camera->set_position({0, 38, 42});
  const Vec3 direction = Normalize(Vec3{0, 0, -12} - ctx.camera->position());
  ctx.camera->set_yaw_pitch(std::atan2(direction.x, -direction.z), std::asin(direction.y));
  ctx.camera->speed = 15.0f;
  RX_INFO("feature gym: nine self-contained districts ready; use --feature-tour for regression");
}

void FeatureGym::Impl::CreateBase() {
  asset::Material floor;
  floor.id = asset::MakeAssetId("featuregym/material/floor");
  floor.base_color = checker;
  floor.base_color_factor[0] = 0.55f;
  floor.base_color_factor[1] = 0.60f;
  floor.base_color_factor[2] = 0.66f;
  floor.roughness_factor = 0.93f;
  if (!headless) renderer.UploadMaterial(floor);
  neutral_material = AddMaterial("neutral", {0.46f, 0.49f, 0.54f}, 0.72f);

  SpawnBox("world_floor", {48, 0.25f, 50}, {0, -0.25f, -12}, floor.id, true, true);
  for (size_t i = 0; i < std::size(kAreas); ++i) {
    const AreaInfo& info = kAreas[i];
    const Vec3 tint{static_cast<f32>((info.color >> 24) & 0xff) / 255.0f,
                    static_cast<f32>((info.color >> 16) & 0xff) / 255.0f,
                    static_cast<f32>((info.color >> 8) & 0xff) / 255.0f};
    asset::AssetId pad = AddMaterial("pad_" + std::to_string(i), tint * 0.22f, 0.88f);
    SpawnBox("pad_" + std::to_string(i), {12.5f, 0.08f, 10.5f},
              info.center + Vec3{0, 0.02f, 0}, pad, false, true);

    const f32 x0 = info.center.x - 12.3f;
    const f32 x1 = info.center.x + 12.3f;
    const f32 z0 = info.center.z - 10.3f;
    const f32 z1 = info.center.z + 10.3f;
    for (const auto& edge : std::array<std::pair<Vec3, Vec3>, 4>{
             std::pair{Vec3{x0, 0.14f, z0}, Vec3{x1, 0.14f, z0}},
             std::pair{Vec3{x1, 0.14f, z0}, Vec3{x1, 0.14f, z1}},
             std::pair{Vec3{x1, 0.14f, z1}, Vec3{x0, 0.14f, z1}},
             std::pair{Vec3{x0, 0.14f, z1}, Vec3{x0, 0.14f, z0}},
         })
      lines.push_back({edge.first, edge.second, info.color});
  }
  lines.push_back({{-47, 0.2f, -12}, {47, 0.2f, -12}, 0x4b5563ff});
  lines.push_back({{0, 0.2f, -61}, {0, 0.2f, 37}, 0x4b5563ff});
}

void FeatureGym::Impl::CreateMaterials() {
  const Vec3 c = Info(Area::kMaterials).center;
  int index = 0;
  auto sphere = [&](Vec3 position, asset::Material material) {
    const std::string stem = "material_" + std::to_string(index++);
    material.id = asset::MakeAssetId("featuregym/material/" + stem);
    asset::Mesh mesh = asset::MakeSphere(0.72f, 28, 40,
                                          asset::MakeAssetId("featuregym/mesh/" + stem));
    mesh = WithMaterial(std::move(mesh), material.id);
    if (ctx.config->renderer.software_gi) mesh.exclude_from_rt = true;
    if (!headless) {
      renderer.UploadMaterial(material);
      renderer.UploadMesh(mesh);
    }
    Spawn(mesh.id, position);
    SpawnBox(stem + "_pedestal", {0.9f, 0.18f, 0.9f}, position - Vec3{0, 0.72f, 0},
             neutral_material);
  };

  for (int x = 0; x < 5; ++x) {
    const f32 t = static_cast<f32>(x) / 4.0f;
    asset::Material material;
    material.base_color_factor[0] = 0.76f;
    material.base_color_factor[1] = 0.16f + t * 0.55f;
    material.base_color_factor[2] = 0.09f;
    material.metallic_factor = t;
    material.roughness_factor = 0.05f + t * 0.9f;
    sphere(c + Vec3{-8 + x * 4.0f, 1.0f, 6.5f}, material);
  }

  asset::Material textured;
  textured.base_color = albedo;
  textured.normal = normal;
  textured.metallic_roughness = orm;
  textured.occlusion_map = orm;
  textured.emissive = emissive;
  textured.emissive_factor[0] = 0.22f;
  textured.emissive_factor[1] = 0.12f;
  textured.emissive_factor[2] = 0.05f;
  sphere(c + Vec3{-8, 1, 2.8f}, textured);

  asset::Material split_left;
  split_left.id = asset::MakeAssetId("featuregym/material/split_pbr_left");
  split_left.base_color = albedo;
  split_left.normal = normal;
  split_left.metallic_roughness = roughness_map;
  split_left.metallic_map = metallic_map;
  split_left.occlusion_map = ao_map;
  split_left.separate_metallic = true;
  split_left.metallic_factor = 1.0f;
  split_left.roughness_factor = 1.0f;
  split_left.ao_strength = 1.0f;
  asset::Material split_right = split_left;
  split_right.id = asset::MakeAssetId("featuregym/material/split_pbr_right");
  split_right.base_color_factor[0] = 0.28f;
  split_right.base_color_factor[1] = 0.72f;
  split_right.base_color_factor[2] = 1.0f;
  split_right.metallic_factor = 0.52f;

  asset::Mesh split_mesh =
      asset::MakeCube(0.78f, asset::MakeAssetId("featuregym/mesh/split_pbr_submeshes"));
  asset::Mesh right_mesh =
      asset::MakeCube(0.78f, asset::MakeAssetId("featuregym/mesh/split_pbr_submeshes_right"));
  asset::MeshLod& split_lod = split_mesh.lods[0];
  asset::MeshLod& right_lod = right_mesh.lods[0];
  for (asset::Vertex& vertex : split_lod.vertices) vertex.position[0] -= 0.95f;
  for (asset::Vertex& vertex : right_lod.vertices) vertex.position[0] += 0.95f;
  const u32 right_vertex_offset = static_cast<u32>(split_lod.vertices.size());
  const u32 right_index_offset = static_cast<u32>(split_lod.indices.size());
  split_lod.vertices.insert(split_lod.vertices.end(), right_lod.vertices.begin(),
                            right_lod.vertices.end());
  for (u32 index_value : right_lod.indices)
    split_lod.indices.push_back(index_value + right_vertex_offset);
  split_lod.submeshes.clear();
  split_lod.submeshes.push_back({0, right_index_offset, split_left.id});
  split_lod.submeshes.push_back(
      {right_index_offset, static_cast<u32>(right_lod.indices.size()), split_right.id});
  split_mesh.bounds_radius = 2.0f;
  if (ctx.config->renderer.software_gi) split_mesh.exclude_from_rt = true;
  if (!headless) {
    renderer.UploadMaterial(split_left);
    renderer.UploadMaterial(split_right);
    renderer.UploadMesh(split_mesh);
  }
  Spawn(split_mesh.id, c + Vec3{-7.0f, 1.05f, -5.0f});
  SpawnBox("split_pbr_pedestal", {2.1f, 0.18f, 1.05f}, c + Vec3{-7.0f, 0.18f, -5.0f},
           neutral_material);

  asset::Material coat;
  coat.base_color_factor[0] = 0.05f;
  coat.base_color_factor[1] = 0.12f;
  coat.base_color_factor[2] = 0.55f;
  coat.roughness_factor = 0.24f;
  coat.clearcoat = 1;
  coat.clearcoat_roughness = 0.04f;
  sphere(c + Vec3{-4, 1, 2.8f}, coat);

  asset::Material anisotropic;
  anisotropic.base_color_factor[0] = 0.86f;
  anisotropic.base_color_factor[1] = 0.64f;
  anisotropic.base_color_factor[2] = 0.18f;
  anisotropic.metallic_factor = 1;
  anisotropic.roughness_factor = 0.34f;
  anisotropic.anisotropy = 0.9f;
  sphere(c + Vec3{0, 1, 2.8f}, anisotropic);

  asset::Material sheen;
  sheen.base_color_factor[0] = 0.12f;
  sheen.base_color_factor[1] = 0.05f;
  sheen.base_color_factor[2] = 0.30f;
  sheen.roughness_factor = 0.88f;
  sheen.sheen_color[0] = 0.9f;
  sheen.sheen_color[1] = 0.45f;
  sheen.sheen_color[2] = 0.75f;
  sheen.sheen_roughness = 0.35f;
  sphere(c + Vec3{4, 1, 2.8f}, sheen);

  asset::Material glass;
  glass.base_color_factor[0] = 0.65f;
  glass.base_color_factor[1] = 0.90f;
  glass.base_color_factor[2] = 0.96f;
  glass.base_color_factor[3] = 0.38f;
  glass.roughness_factor = 0.04f;
  glass.transmission = 0.92f;
  glass.ior = 1.52f;
  glass.alpha_mode = asset::AlphaMode::kBlend;
  glass.two_sided = true;
  sphere(c + Vec3{8, 1, 2.8f}, glass);

  asset::Material skin_mat;
  skin_mat.base_color_factor[0] = 0.78f;
  skin_mat.base_color_factor[1] = 0.48f;
  skin_mat.base_color_factor[2] = 0.36f;
  skin_mat.roughness_factor = 0.52f;
  skin_mat.subsurface = 0.7f;
  skin_mat.skin = true;
  sphere(c + Vec3{-8, 1, -1}, skin_mat);

  asset::Material iridescent;
  iridescent.base_color_factor[0] = 0.025f;
  iridescent.base_color_factor[1] = 0.025f;
  iridescent.base_color_factor[2] = 0.035f;
  iridescent.roughness_factor = 0.12f;
  iridescent.iridescence = 1;
  iridescent.iridescence_thickness = 720;
  sphere(c + Vec3{-4, 1, -1}, iridescent);

  asset::Material model_normal;
  model_normal.base_color_factor[0] = 0.45f;
  model_normal.base_color_factor[1] = 0.65f;
  model_normal.base_color_factor[2] = 0.82f;
  model_normal.normal = object_normal;
  model_normal.normal_model_space = true;
  sphere(c + Vec3{0, 1, -1}, model_normal);

  asset::Material pom;
  pom.base_color = albedo;
  pom.normal = normal;
  pom.height = height;
  pom.height_scale = 0.075f;
  pom.silhouette_pom = true;
  pom.silhouette_curvature = 0.9f;
  pom.roughness_factor = 0.72f;
  sphere(c + Vec3{4, 1, -1}, pom);

  asset::Material masked;
  masked.base_color = alpha;
  masked.alpha_mode = asset::AlphaMode::kMask;
  masked.alpha_cutoff = 0.5f;
  masked.two_sided = true;
  sphere(c + Vec3{8, 1, -1}, masked);

  asset::Material effect;
  effect.base_color = emissive;
  effect.base_color_factor[0] = 1.0f;
  effect.base_color_factor[1] = 0.35f;
  effect.base_color_factor[2] = 0.06f;
  effect.effect = true;
  effect.effect_additive = true;
  effect.effect_falloff = true;
  effect.emissive_pulse[0] = 1.2f;
  effect.emissive_pulse[1] = 0.45f;
  effect.alpha_mode = asset::AlphaMode::kBlend;
  effect.two_sided = true;
  sphere(c + Vec3{-4, 1, -5}, effect);

  asset::Material terrain;
  terrain.id = asset::MakeAssetId("featuregym/material/terrain_splat");
  terrain.is_terrain = true;
  terrain.base_color = checker;
  terrain.normal = normal;
  terrain.metallic_roughness = orm;
  terrain.emissive = weights;
  terrain.height = weights_b;
  terrain.height_scale = 0;
  terrain.terrain_layer_count = 8;
  const asset::AssetId terrain_layers[] = {checker, albedo, alpha, emissive,
                                            albedo, checker, emissive, alpha};
  for (u32 i = 0; i < 8; ++i) {
    terrain.terrain_layers[i] = terrain_layers[i];
    terrain.terrain_layer_normals[i] = normal;
  }
  terrain.roughness_factor = 0.9f;
  if (!headless) renderer.UploadMaterial(terrain);
  SpawnBox("terrain_splat", {4.0f, 0.12f, 2.5f}, c + Vec3{4, 0.25f, -5}, terrain.id);

  if (!headless) renderer.SetDecalAtlas(decal, decal_normal);
  decals.push_back(MakeDecal(c + Vec3{4, 0.5f, -2.45f}, {0, 0, 1}, {0, 1, 0}, 1.1f, 1.1f));
  decals.push_back(MakeDecal(c + Vec3{7, 0.5f, -4.8f}, {0, 1, 0}, {0, 0, 1}, 1.3f, 1.3f));
}

void FeatureGym::Impl::CreateLighting() {
  const Vec3 c = Info(Area::kLighting).center;
  asset::AssetId white = AddMaterial("light_receiver", {0.30f, 0.31f, 0.34f}, 0.48f);
  for (int i = 0; i < 7; ++i) {
    asset::Mesh sphere = asset::MakeSphere(0.65f, 20, 28,
                                           asset::MakeAssetId("featuregym/mesh/light_receiver_" +
                                                              std::to_string(i)));
    const asset::AssetId id = UploadMesh(std::move(sphere), white, true);
    Spawn(id, c + Vec3{-9 + i * 3.0f, 0.75f, -1.0f + (i & 1) * 2.0f});
  }
  for (int i = 0; i < 6; ++i)
    SpawnBox("light_pillar_" + std::to_string(i), {0.22f, 1.2f, 0.35f},
              c + Vec3{-7.5f + i * 3.0f, 1.2f, -3.0f}, white, false, true);

  const Vec3 colors[] = {{1, 0.12f, 0.08f}, {0.08f, 1, 0.20f}, {0.12f, 0.35f, 1},
                         {1, 0.75f, 0.12f}, {0.8f, 0.12f, 1}, {0.05f, 0.85f, 1}};
  for (int i = 0; i < 6; ++i) {
    render::PointLight light;
    light.pos_radius[0] = c.x - 7.5f + i * 3.0f;
    light.pos_radius[1] = 2.1f;
    light.pos_radius[2] = c.z + (i & 1 ? 1.5f : -0.2f);
    light.pos_radius[3] = 5.0f;
    light.color_intensity[0] = colors[i].x;
    light.color_intensity[1] = colors[i].y;
    light.color_intensity[2] = colors[i].z;
    light.color_intensity[3] = 8.0f;
    lights.push_back(light);
  }
  render::PointLight spot;
  spot.pos_radius[0] = c.x - 8;
  spot.pos_radius[1] = 5;
  spot.pos_radius[2] = c.z + 6;
  spot.pos_radius[3] = 14;
  spot.color_intensity[0] = 1;
  spot.color_intensity[1] = 0.86f;
  spot.color_intensity[2] = 0.62f;
  spot.color_intensity[3] = 22;
  Vec3 spot_direction = Normalize(Vec3{0.35f, -0.85f, -0.4f});
  spot.direction_type[0] = spot_direction.x;
  spot.direction_type[1] = spot_direction.y;
  spot.direction_type[2] = spot_direction.z;
  spot.direction_type[3] = 1;
  spot.params[0] = std::cos(0.28f);
  spot.params[1] = std::cos(0.48f);
  lights.push_back(spot);

  render::PointLight sphere;
  sphere.pos_radius[0] = c.x + 7;
  sphere.pos_radius[1] = 2.0f;
  sphere.pos_radius[2] = c.z + 5;
  sphere.pos_radius[3] = 9;
  sphere.color_intensity[0] = 1;
  sphere.color_intensity[1] = 0.42f;
  sphere.color_intensity[2] = 0.12f;
  sphere.color_intensity[3] = 12;
  sphere.direction_type[3] = 2;
  sphere.params[0] = 0.55f;
  lights.push_back(sphere);

  render::PointLight panel;
  panel.pos_radius[0] = c.x;
  panel.pos_radius[1] = 3.4f;
  panel.pos_radius[2] = c.z - 6;
  panel.pos_radius[3] = 12;
  panel.color_intensity[0] = 0.34f;
  panel.color_intensity[1] = 0.62f;
  panel.color_intensity[2] = 1;
  panel.color_intensity[3] = 10;
  panel.direction_type[2] = 1;
  panel.direction_type[3] = 3;
  panel.params[0] = 2.4f;
  panel.params[1] = 1.0f;
  lights.push_back(panel);
}

void FeatureGym::Impl::CreateGeometry() {
  const Vec3 c = Info(Area::kGeometry).center;
  asset::AssetId blue = AddMaterial("geometry", {0.15f, 0.46f, 0.78f}, 0.38f);
  asset::Mesh lod = asset::MakeLodSphere(1.15f, asset::MakeAssetId("featuregym/mesh/lod"));
  const asset::AssetId lod_id = UploadMesh(std::move(lod), blue);
  for (int i = 0; i < 5; ++i) Spawn(lod_id, c + Vec3{-10 + i * 4.2f, 1.25f, 6});

  asset::Mesh auto_lod = asset::MakeSphere(1.0f, 60, 90,
                                           asset::MakeAssetId("featuregym/mesh/auto_lod"));
  auto_lod = WithMaterial(std::move(auto_lod), blue);
  asset::GenerateLods(&auto_lod);
  if (ctx.config->renderer.software_gi) auto_lod.exclude_from_rt = true;
  const asset::AssetId auto_id = auto_lod.id;
  if (!headless) renderer.UploadMesh(auto_lod);
  for (int i = 0; i < 4; ++i) Spawn(auto_id, c + Vec3{-8 + i * 5.0f, 1.1f, 2.5f});

  SpawnBox("occlusion_wall", {3.8f, 2.2f, 0.2f}, c + Vec3{-6, 2.2f, -1.5f},
            neutral_material, false, true);
  asset::AssetId small = UploadMesh(asset::MakeCube(0.16f,
                                                     asset::MakeAssetId("featuregym/mesh/occluded")),
                                    AddMaterial("occluded", {0.8f, 0.18f, 0.12f}, 0.5f));
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 12; ++x)
      Spawn(small, c + Vec3{-8.6f + x * 0.48f, 0.5f + y * 0.45f, -4.0f - (x % 3) * 0.4f});

  asset::Material vt;
  vt.id = asset::MakeAssetId("featuregym/material/virtual_texture");
  vt.virtual_albedo = true;
  vt.roughness_factor = 0.9f;
  if (!headless) renderer.UploadMaterial(vt);
  SpawnBox("virtual_texture", {3.8f, 0.1f, 2.2f}, c + Vec3{7.5f, 0.18f, -4.8f}, vt.id);

  asset::Mesh meshlet = asset::MakeSphere(1.6f, 56, 96,
                                          asset::MakeAssetId("featuregym/mesh/meshlet"));
  const Vec3 meshlet_center = c + Vec3{8, 2.0f, 0};
  for (asset::Vertex& v : meshlet.lods[0].vertices) {
    v.position[0] += meshlet_center.x;
    v.position[1] += meshlet_center.y;
    v.position[2] += meshlet_center.z;
  }
  meshlet.bounds_center[0] = meshlet_center.x;
  meshlet.bounds_center[1] = meshlet_center.y;
  meshlet.bounds_center[2] = meshlet_center.z;
  if (!headless) renderer.UploadMeshletMesh(meshlet);

  asset::Mesh terrain;
  terrain.id = asset::MakeAssetId("featuregym/mesh/virtual_geometry");
  terrain.lods.emplace_back();
  asset::MeshLod& terrain_lod = terrain.lods[0];
  constexpr u32 kGrid = 96;
  for (u32 z = 0; z <= kGrid; ++z) {
    for (u32 x = 0; x <= kGrid; ++x) {
      const f32 px = (static_cast<f32>(x) / kGrid - 0.5f) * kVirtualGeometrySize;
      const f32 pz = (static_cast<f32>(z) / kGrid - 0.5f) * kVirtualGeometrySize;
      asset::Vertex vertex{};
      vertex.position[0] = px;
      vertex.position[1] = std::sin(px * 1.4f) * std::cos(pz * 1.2f) * 0.6f;
      vertex.position[2] = pz;
      vertex.normal[1] = 1;
      terrain_lod.vertices.push_back(vertex);
    }
  }
  for (u32 z = 0; z < kGrid; ++z) {
    for (u32 x = 0; x < kGrid; ++x) {
      const u32 a = z * (kGrid + 1) + x;
      const u32 b = a + 1;
      const u32 d = a + kGrid + 1;
      const u32 e = d + 1;
      for (u32 index : {a, d, b, b, d, e}) terrain_lod.indices.push_back(index);
    }
  }
  if (!headless) {
    renderer.UploadVirtualGeometryMesh(terrain);
    if (!virtual_geometry_albedo.empty()) {
      renderer.SetVirtualGeometryAlbedo(
          ByteSpan(virtual_geometry_albedo.data(), virtual_geometry_albedo.size()), kTextureSize,
          1.0f / kVirtualGeometrySize);
    }
    const Mat4 instance = MakeTranslation(c + Vec3{0, 0.4f, -6});
    renderer.SetVirtualGeometryInstances(std::span<const Mat4>(&instance, 1));
  }

  asset::Mesh tree = MakeTree(asset::MakeAssetId("featuregym/mesh/tree"),
                               AddMaterial("tree", {0.22f, 0.56f, 0.20f}, 0.9f));
  if (!headless) renderer.UploadMesh(tree);
  for (int i = 0; i < 5; ++i) Spawn(tree.id, c + Vec3{-10 + i * 2.8f, 0.1f, -8});

  if (!headless) {
    asset::Mesh prop = asset::MakeCube(0.36f, asset::MakeAssetId("featuregym/mesh/instance_prop"));
    prop = WithMaterial(std::move(prop),
                        AddMaterial("instance_prop", {0.06f, 0.72f, 0.92f}, 0.28f, 0.45f));
    renderer.UploadMesh(prop);
    prop_mesh = prop.id.hash;
    prop_transforms.reserve(30);
    prop_updated_transforms.reserve(36);
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 10; ++column) {
        const f32 scale = 0.72f + static_cast<f32>((row + column) % 4) * 0.09f;
        const Vec3 position = c + Vec3{-10.0f + column * 2.2f, 0.45f, -1.0f - row * 2.1f};
        prop_transforms.push_back(MakeTranslation(position) * MakeScale(scale));
        prop_updated_transforms.push_back(
            MakeTranslation(position + Vec3{0, 0.35f + 0.12f * std::sin(column * 0.7f), -0.45f}) *
            MakeFromQuat(QuatFromAxisAngle({0, 1, 0}, 0.18f * (row + column))) *
            MakeScale(scale));
      }
    }
    for (int column = 0; column < 6; ++column) {
      const Vec3 position = c + Vec3{-5.5f + column * 2.2f, 1.35f, -5.6f};
      prop_updated_transforms.push_back(MakeTranslation(position) * MakeScale(0.68f));
    }
    prop_group = renderer.CreateInstanceGroup(prop_mesh, prop_transforms);
    if (!prop_group) RX_WARN("feature gym: persistent instance group unavailable");
  }

  std::vector<render::ImposterPass::Instance> imposters;
  for (int i = 0; i < 80; ++i) {
    const f32 angle = i * 2.39996323f;
    const f32 radius = 10.0f + (i % 9) * 0.65f;
    render::ImposterPass::Instance instance;
    instance.position[0] = c.x + std::cos(angle) * radius;
    instance.position[1] = 0.1f;
    instance.position[2] = c.z + std::sin(angle) * radius;
    instance.scale = 0.65f + (i % 5) * 0.08f;
    imposters.push_back(instance);
  }
  if (!headless) renderer.BakeImposter(tree, imposters);
}

void FeatureGym::Impl::CreateAtmosphere() {
  const Vec3 c = Info(Area::kAtmosphere).center;
  asset::AssetId tower = AddMaterial("atmosphere_tower", {0.34f, 0.28f, 0.48f}, 0.78f);
  for (int i = 0; i < 9; ++i) {
    const f32 height_value = 1.2f + i * 0.65f;
    SpawnBox("atmosphere_tower_" + std::to_string(i), {0.65f, height_value, 0.65f},
             c + Vec3{-9 + i * 2.25f, height_value, -5.5f}, tower);
  }
  asset::Material cloth;
  cloth.id = asset::MakeAssetId("featuregym/material/weather_cloth");
  cloth.base_color = albedo;
  cloth.base_color_factor[0] = 0.55f;
  cloth.base_color_factor[1] = 0.22f;
  cloth.base_color_factor[2] = 0.82f;
  cloth.roughness_factor = 0.8f;
  cloth.sheen_color[0] = 0.7f;
  cloth.sheen_color[1] = 0.5f;
  cloth.sheen_color[2] = 1.0f;
  cloth.wind = true;
  cloth.two_sided = true;
  if (!headless) renderer.UploadMaterial(cloth);
  asset::Mesh banner = MakeBanner(asset::MakeAssetId("featuregym/mesh/weather_banner"), cloth.id);
  if (!headless) renderer.UploadMesh(banner);
  for (int i = 0; i < 5; ++i) {
    SpawnBox("weather_pole_" + std::to_string(i), {0.07f, 2.7f, 0.07f},
             c + Vec3{-8 + i * 4.0f, 2.7f, 4.5f}, neutral_material);
    Spawn(banner.id, c + Vec3{-6.8f + i * 4.0f, 5.1f, 4.5f});
  }
}

void FeatureGym::Impl::CreateWater() {
  const Vec3 c = Info(Area::kWater).center;
  asset::Material water;
  water.id = asset::MakeAssetId("featuregym/material/water");
  water.base_color_factor[0] = 0.035f;
  water.base_color_factor[1] = 0.20f;
  water.base_color_factor[2] = 0.30f;
  water.base_color_factor[3] = 0.78f;
  water.roughness_factor = 0.08f;
  water.alpha_mode = asset::AlphaMode::kBlend;
  water.two_sided = true;
  water.is_water = true;
  if (!headless) renderer.UploadMaterial(water);
  asset::Mesh water_mesh = MakeWaterGrid({c.x, water_height, c.z}, 10.0f, 64,
                                         asset::MakeAssetId("featuregym/mesh/water"), water.id);
  if (!headless) renderer.UploadMesh(water_mesh);
  Spawn(water_mesh.id, {});

  asset::AssetId sand = AddMaterial("water_floor", {0.28f, 0.34f, 0.27f}, 0.95f);
  SpawnBox("water_floor", {10.5f, 0.15f, 10.5f}, c + Vec3{0, -2.3f, 0}, sand, true, true);
  constexpr f32 kIslandSigma = 2.8f;
  constexpr f32 kIslandPeak = 0.75f;
  constexpr f32 kIslandRadius = 5.0f;
  constexpr u32 kIslandGrid = 40;
  const Vec3 island_center = c + Vec3{5.0f, 0, -2.0f};
  asset::Mesh island;
  island.id = asset::MakeAssetId("featuregym/mesh/shore_island");
  asset::MeshLod& island_lod = island.lods.emplace_back();
  for (u32 z = 0; z <= kIslandGrid; ++z) {
    for (u32 x = 0; x <= kIslandGrid; ++x) {
      const f32 local_x =
          -kIslandRadius + 2.0f * kIslandRadius * static_cast<f32>(x) / kIslandGrid;
      const f32 local_z =
          -kIslandRadius + 2.0f * kIslandRadius * static_cast<f32>(z) / kIslandGrid;
      const f32 gaussian =
          std::exp(-(local_x * local_x + local_z * local_z) /
                   (2.0f * kIslandSigma * kIslandSigma));
      const f32 slope = kIslandPeak * 2.0f * gaussian / (kIslandSigma * kIslandSigma);
      const Vec3 island_normal = Normalize(Vec3{slope * local_x, 1, slope * local_z});
      asset::Vertex vertex{};
      vertex.position[0] = local_x;
      vertex.position[1] = water_height + kIslandPeak * (2.0f * gaussian - 1.0f);
      vertex.position[2] = local_z;
      vertex.normal[0] = island_normal.x;
      vertex.normal[1] = island_normal.y;
      vertex.normal[2] = island_normal.z;
      vertex.tangent[0] = 1;
      vertex.tangent[3] = 1;
      vertex.uv[0] = local_x / 3.0f;
      vertex.uv[1] = local_z / 3.0f;
      island_lod.vertices.push_back(vertex);
    }
  }
  for (u32 z = 0; z < kIslandGrid; ++z) {
    for (u32 x = 0; x < kIslandGrid; ++x) {
      const u32 a = z * (kIslandGrid + 1) + x;
      const u32 b = a + 1;
      const u32 d = a + kIslandGrid + 1;
      const u32 e = d + 1;
      for (u32 index_value : {a, b, d, b, e, d}) island_lod.indices.push_back(index_value);
    }
  }
  island_lod.submeshes.push_back({0, static_cast<u32>(island_lod.indices.size()), sand});
  island.bounds_radius = kIslandRadius * 1.5f;
  if (ctx.config->renderer.software_gi) island.exclude_from_rt = true;
  if (!headless) renderer.UploadMesh(island);
  Spawn(island.id, island_center);

  physics.set_water_height([this, c](const Vec3& p, f32* out, Vec3* flow) {
    if (std::abs(p.x - c.x) > 11 || std::abs(p.z - c.z) > 11) return false;
    Vec3 orbital_flow{};
    *out = water_height + physics::GerstnerWaveHeight(p.x, p.z, sim_time, &orbital_flow);
    if (flow) *flow = orbital_flow;
    return true;
  });
  asset::AssetId floater_material = AddMaterial("floater", {0.70f, 0.35f, 0.08f}, 0.74f);
  const asset::AssetId cube = UploadMesh(asset::MakeCube(0.55f,
                                                         asset::MakeAssetId("featuregym/mesh/floater")),
                                        floater_material);
  for (int i = 0; i < 5; ++i) {
    const Vec3 position = c + Vec3{-5 + i * 2.5f, 1.2f + (i & 1), (i % 3 - 1) * 2.2f};
    ecs::Entity entity = Spawn(cube, position);
    physics::BodyId body = physics.AddDynamicBox(position, {0.55f, 0.55f, 0.55f}, 420, {});
    if (body) {
      ctx.physics_entities->push_back({body, entity});
      water_bodies.push_back(body);
      water_previous.push_back(position);
    }
  }
}

void FeatureGym::Impl::CreateEffects() {
  const Vec3 c = Info(Area::kEffects).center;
  asset::AssetId dark = AddMaterial("effects_floor", {0.08f, 0.09f, 0.12f}, 0.72f);
  SpawnBox("effects_stage", {11.5f, 0.12f, 9.5f}, c + Vec3{0, 0.17f, 0}, dark, false, true);

  for (int i = 0; i < 5; ++i) {
    render::WboitInstance instance;
    instance.model = MakeTranslation(c + Vec3{-4 + i * 2.0f, 1.4f, -4 + (i & 1) * 0.8f}) *
                     MakeScale(1.05f);
    instance.color[0] = i == 0 || i == 3 ? 1.0f : 0.12f;
    instance.color[1] = i == 1 || i == 3 ? 1.0f : 0.18f;
    instance.color[2] = i == 2 || i == 4 ? 1.0f : 0.22f;
    instance.color[3] = 0.48f;
    oit.push_back(instance);
  }

  constexpr u32 kSplatCount = 3200;
  for (u32 i = 0; i < kSplatCount; ++i) {
    const f32 t = (i + 0.5f) / kSplatCount;
    const f32 y = 1.0f - 2.0f * t;
    const f32 radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const f32 angle = i * 2.39996323f;
    const Vec3 direction{std::cos(angle) * radius, y, std::sin(angle) * radius};
    render::GaussianInstance splat;
    splat.position[0] = c.x + 5.2f + direction.x * 1.6f;
    splat.position[1] = 2.0f + direction.y * 1.6f;
    splat.position[2] = c.z - 3.8f + direction.z * 1.6f;
    splat.scale[0] = splat.scale[1] = splat.scale[2] = 0.075f;
    splat.rotation[3] = 1;
    splat.color[0] = direction.x * 0.5f + 0.5f;
    splat.color[1] = direction.y * 0.5f + 0.5f;
    splat.color[2] = direction.z * 0.5f + 0.5f;
    splat.opacity = 0.82f;
    gaussians.push_back(splat);
  }
  if (!headless) {
    render::GroomData groom;
    if (render::BuildTestGroom(render::TestGroomStyle::kPonytail, 180, 11, &groom)) {
      constexpr f32 kGroomScale = 7.5f;
      for (f32& value : groom.points) value *= kGroomScale;
      for (f32& value : groom.roots) value *= kGroomScale;
      groom.collision_center = groom.collision_center * kGroomScale;
      groom.collision_radius *= kGroomScale;
      groom.mean_length *= kGroomScale;
      groom.authored_scalp = groom.authored_scalp * kGroomScale;
      groom.sim.node_radius *= kGroomScale;

      strand_center = c + Vec3{7.0f, 2.15f, 4.5f};
      strand_transform = MakeTranslation(strand_center);
      render::GroomParams params;
      params.children_per_guide = 14;
      params.clump_radius = 0.018f;
      params.strand_width = 0.0045f;
      params.tint = {1.0f, 0.72f, 0.24f};
      strand_groom = renderer.CreateHairGroom(groom, params, strand_transform);

      physics::PhysicsWorld::StrandGroomDesc desc = ToStrandGroomDesc(groom);
      physics::PhysicsWorld::StrandGroomDesc::Sphere head{groom.collision_center,
                                                          groom.collision_radius};
      physics::PhysicsWorld::StrandGroomDesc::Capsule shoulders{
          {-1.05f, -2.25f, 0}, {1.05f, -2.25f, 0}, 0.52f};
      desc.spheres = &head;
      desc.sphere_count = 1;
      desc.capsules = &shoulders;
      desc.capsule_count = 1;
      strand_sim = physics.CreateStrandGroom(desc, strand_transform);
      if (strand_sim)
        strand_positions.resize(physics.StrandGroomPositionCount(strand_sim));

      Vec3 head_center = strand_center;
      f32 head_radius = groom.collision_radius;
      if (strand_groom) renderer.HairGroomHead(strand_groom, &head_center, &head_radius);
      const asset::AssetId head_material =
          AddMaterial("strand_head", {0.58f, 0.34f, 0.24f}, 0.55f);
      const asset::AssetId head_mesh = UploadMesh(
          asset::MakeSphere(head_radius * 0.92f, 24, 36,
                            asset::MakeAssetId("featuregym/mesh/strand_head")),
          head_material);
      Spawn(head_mesh, head_center);
    }
  }
}

void FeatureGym::Impl::CreateNetworkBubbles() {
#if RECREATION_HAS_NET
  const Vec3 c = Info(Area::kEffects).center;
  const asset::AssetId pawn_mesh = UploadMesh(
      asset::MakeCube(0.28f, asset::MakeAssetId("featuregym/mesh/network_pawn")),
      AddMaterial("network_pawn", {0.34f, 0.36f, 0.40f}, 0.72f));
  const asset::AssetId player_mesh = UploadMesh(
      asset::MakeCube(0.55f, asset::MakeAssetId("featuregym/mesh/network_player")),
      AddMaterial("network_player", {0.92f, 0.92f, 0.96f}, 0.38f));
  for (int z = -3; z <= 3; ++z) {
    for (int x = -4; x <= 4; ++x) {
      ecs::Entity entity = Spawn(pawn_mesh, c + Vec3{x * 2.1f, 0.55f, z * 2.1f});
      world.Add(entity, net::AllocateNetworkId());
      world.Add(entity, scene::Tint{0x555555});
    }
  }
  for (u32 peer = 1; peer <= 2; ++peer) {
    ecs::Entity entity = Spawn(player_mesh, c + Vec3{peer == 1 ? -3.0f : 3.0f, 1.0f, 0});
    world.Add(entity, net::AllocateNetworkId());
    world.Add(entity, net::InterestBubble{peer, 5.8f});
    world.Add(entity, scene::Tint{net::PeerColor(peer)});
    world.Add(entity, BubbleAgent{.time = peer * 2.4f,
                                  .rate_x = 0.22f + peer * 0.06f,
                                  .rate_z = 0.31f - peer * 0.05f,
                                  .extent = 5.0f,
                                  .center = c + Vec3{0, 1.0f, 0}});
  }
  bubble_map.Configure({.hysteresis = 1.15f, .cell_size = 4.0f});
  if (!headless) {
    bubble_viz = std::make_unique<net::BubbleVisualizer>();
    if (!bubble_viz->Init(renderer)) bubble_viz.reset();
  }
#endif
}

void FeatureGym::Impl::CreatePhysics() {
  const Vec3 c = Info(Area::kPhysics).center;
  if (!physics.initialized()) {
    RX_WARN("feature gym: physics district is static; rebuild with Jolt for simulation");
  }
  asset::AssetId orange = AddMaterial("physics_dynamic", {0.88f, 0.36f, 0.08f}, 0.55f);
  const asset::AssetId cube = UploadMesh(asset::MakeCube(0.48f,
                                                         asset::MakeAssetId("featuregym/mesh/dynamic_box")),
                                        orange);
  const asset::AssetId sphere = UploadMesh(
      asset::MakeSphere(0.52f, 20, 28, asset::MakeAssetId("featuregym/mesh/dynamic_sphere")),
      AddMaterial("physics_sphere", {0.22f, 0.68f, 0.30f}, 0.42f));
  for (int i = 0; i < 6; ++i) {
    const Vec3 position = c + Vec3{-9 + i * 1.25f, 1.0f + i * 0.95f, 5};
    ecs::Entity entity = Spawn(i & 1 ? sphere : cube, position);
    physics::BodyId body = i & 1 ? physics.AddDynamicSphere(position, 0.52f, 760, {})
                                 : physics.AddDynamicBox(position, {0.48f, 0.48f, 0.48f}, 680, {});
    if (body) ctx.physics_entities->push_back({body, entity});
  }

  physics::ShapeDesc compound;
  compound.kind = physics::ShapeDesc::Kind::kCompound;
  physics::ShapeDesc compound_box;
  compound_box.kind = physics::ShapeDesc::Kind::kBox;
  compound_box.half_extents = {0.8f, 0.25f, 0.35f};
  physics::ShapeDesc compound_sphere;
  compound_sphere.kind = physics::ShapeDesc::Kind::kSphere;
  compound_sphere.radius = 0.42f;
  compound.children.push_back(compound_box);
  compound.children.push_back(compound_sphere);
  const Vec3 compound_position = c + Vec3{0, 3, 5};
  const f32 identity[4] = {0, 0, 0, 1};
  physics::BodyId compound_body =
      physics.AddDynamicShape(compound, compound_position, identity, 1, 18, 0.55f, 0.25f);
  ecs::Entity compound_entity = Spawn(cube, compound_position, 1.45f);
  if (compound_body) ctx.physics_entities->push_back({compound_body, compound_entity});

  const Vec3 hinge_a_pos = c + Vec3{4, 3.2f, 5};
  const Vec3 hinge_b_pos = c + Vec3{4, 1.7f, 5};
  physics::BodyId hinge_a = physics.AddDynamicBox(hinge_a_pos, {0.25f, 0.75f, 0.25f}, 620, {});
  physics::BodyId hinge_b = physics.AddDynamicBox(hinge_b_pos, {0.25f, 0.75f, 0.25f}, 620, {});
  ecs::Entity hinge_a_entity = Spawn(cube, hinge_a_pos, 1.25f);
  ecs::Entity hinge_b_entity = Spawn(cube, hinge_b_pos, 1.25f);
  if (hinge_a) ctx.physics_entities->push_back({hinge_a, hinge_a_entity});
  if (hinge_b) ctx.physics_entities->push_back({hinge_b, hinge_b_entity});
  const f32 frame_a[12] = {1, 0, 0, 0, 0, 1, 0, -0.75f, 0, 0, 1, 0};
  const f32 frame_b[12] = {1, 0, 0, 0, 0, 1, 0, 0.75f, 0, 0, 1, 0};
  physics::JointId hinge =
      physics.AddHingeJoint(hinge_a, hinge_b, frame_a, frame_b, 1, -1.2f, 1.2f);
  if (hinge) {
    physics.EnableJointMotors(hinge, 1.4f, 0.8f);
    const f32 target[4] = {0.25f, 0, 0, 0.9682458f};
    physics.SetJointMotorTarget(hinge, target);
  }

  platform_entity = SpawnBox("moving_platform", {1.8f, 0.25f, 1.5f}, platform_origin,
                             AddMaterial("platform", {0.12f, 0.78f, 0.78f}, 0.48f));
  platform_body = physics.AddKinematicBox(platform_origin, {1.8f, 0.25f, 1.5f});
  character_entity = SpawnBox("character_proxy", {0.32f, 0.9f, 0.32f},
                              c + Vec3{-8, 0.9f, -4},
                              AddMaterial("character_proxy", {0.18f, 0.42f, 0.92f}, 0.52f));
  character = physics.CreateCharacter(c + Vec3{-8, 0.9f, -4}, 0.32f, 0.58f);

  car_mesh = UploadMesh(asset::MakeBox(0.9f, 0.35f, 1.8f,
                                       asset::MakeAssetId("featuregym/mesh/car")),
                        AddMaterial("car", {0.72f, 0.08f, 0.06f}, 0.26f, 0.35f))
                 .hash;
  wheel_mesh = UploadMesh(asset::MakeSphere(0.34f, 14, 18,
                                            asset::MakeAssetId("featuregym/mesh/wheel")),
                          AddMaterial("wheel", {0.035f, 0.038f, 0.045f}, 0.9f))
                   .hash;
  bike_mesh = UploadMesh(asset::MakeBox(0.22f, 0.30f, 0.85f,
                                        asset::MakeAssetId("featuregym/mesh/bike")),
                         AddMaterial("bike", {0.10f, 0.72f, 0.24f}, 0.34f, 0.15f))
                  .hash;
  physics::PhysicsWorld::VehicleDesc car_desc;
  car_desc.drivetrain = physics::PhysicsWorld::Drivetrain::kAWD;
  car_desc.traction_control = true;
  car_desc.downforce = 1.8f;
  car = physics.CreateVehicle(car_desc, c + Vec3{4, 1.0f, -4}, kPi * 0.5f);
  physics::PhysicsWorld::MotorcycleDesc bike_desc;
  bike_desc.traction_control = true;
  bike = physics.CreateMotorcycle(bike_desc, c + Vec3{8, 1.0f, -4}, kPi * 0.5f);

  constexpr u32 kHeightSamples = 17;
  std::array<f32, kHeightSamples * kHeightSamples> heights{};
  for (u32 z = 0; z < kHeightSamples; ++z)
    for (u32 x = 0; x < kHeightSamples; ++x)
      heights[z * kHeightSamples + x] =
          0.25f * std::sin(x * 0.7f) * std::cos(z * 0.55f);
  physics.AddHeightField(c + Vec3{-11, 0, -10}, heights.data(), kHeightSamples, 8.0f);
  CreateCloth();
}

void FeatureGym::Impl::CreateCloth() {
  constexpr u32 kWidth = 11;
  constexpr u32 kHeight = 13;
  constexpr u32 kVertexCount = kWidth * kHeight;
  constexpr f32 kSpacing = 0.14f;
  static_assert(kVertexCount <= 256, "cloth rendering uses one u8 bone per vertex");

  base::Vector<Vec3> rest;
  base::Vector<f32> uvs;
  base::Vector<u32> pins;
  rest.reserve(kVertexCount);
  uvs.reserve(kVertexCount * 2);
  pins.reserve(kWidth);
  cloth_indices.clear();
  cloth_indices.reserve((kWidth - 1) * (kHeight - 1) * 6);
  for (u32 y = 0; y < kHeight; ++y) {
    for (u32 x = 0; x < kWidth; ++x) {
      rest.push_back({(static_cast<f32>(x) - static_cast<f32>(kWidth - 1) * 0.5f) * kSpacing,
                      -static_cast<f32>(y) * kSpacing, 0});
      uvs.push_back(static_cast<f32>(x) / static_cast<f32>(kWidth - 1));
      uvs.push_back(static_cast<f32>(y) / static_cast<f32>(kHeight - 1));
    }
  }
  for (u32 x = 0; x < kWidth; ++x) pins.push_back(x);
  for (u32 y = 0; y + 1 < kHeight; ++y) {
    for (u32 x = 0; x + 1 < kWidth; ++x) {
      const u32 a = y * kWidth + x;
      const u32 b = a + 1;
      const u32 c = a + kWidth;
      const u32 d = c + 1;
      for (u32 index : {a, c, b, b, c, d}) cloth_indices.push_back(index);
    }
  }

  physics::ClothDesc desc;
  desc.positions = rest.data();
  desc.vertex_count = static_cast<u32>(rest.size());
  desc.indices = cloth_indices.data();
  desc.index_count = static_cast<u32>(cloth_indices.size());
  desc.uvs = uvs.data();
  desc.pins = pins.data();
  desc.pin_count = static_cast<u32>(pins.size());
  desc.areal_density = 0.24f;
  desc.shear_compliance = 2.0e-6f;
  desc.bend_compliance = 1.5e-4f;
  desc.iterations = 8;
  desc.damping = 0.07f;
  desc.collision_radius = 0.009f;
  desc.self_collision_distance = 0.018f;
  desc.self_collision_iterations = 3;
  desc.aerodynamic_drag = 1.2f;

  const Vec3 origin = Info(Area::kPhysics).center + Vec3{8.0f, 5.4f, 2.0f};
  cloth_id = physics.CreateCloth(desc, MakeTranslation(origin));
  if (!cloth_id) {
    if (physics.initialized())
      RX_WARN("feature gym: cloth unavailable; physics.cloth may be disabled");
    return;
  }
  physics.SetClothWind(cloth_id, {0.7f, 0.08f, -2.5f});
  cloth_width = kWidth;
  cloth_positions.resize(kVertexCount);
  cloth_normals.resize(kVertexCount);
  cloth_lines.reserve(cloth_indices.size());

  asset::Material fabric;
  fabric.id = asset::MakeAssetId("featuregym/material/simulated_cloth");
  fabric.base_color_factor[0] = 0.04f;
  fabric.base_color_factor[1] = 0.45f;
  fabric.base_color_factor[2] = 0.52f;
  fabric.roughness_factor = 0.62f;
  fabric.sheen_color[0] = 0.15f;
  fabric.sheen_color[1] = 0.55f;
  fabric.sheen_color[2] = 0.62f;
  fabric.sheen_roughness = 0.72f;
  fabric.two_sided = true;

  asset::Mesh mesh;
  mesh.id = asset::MakeAssetId("featuregym/mesh/simulated_cloth");
  mesh.skinned = true;
  mesh.exclude_from_rt = true;
  asset::MeshLod& lod = mesh.lods.emplace_back();
  lod.indices = cloth_indices;
  lod.vertices.reserve(kVertexCount);
  lod.skinning.reserve(kVertexCount);
  for (u32 i = 0; i < kVertexCount; ++i) {
    asset::Vertex vertex{};
    vertex.normal[2] = 1.0f;
    vertex.tangent[0] = 1.0f;
    vertex.tangent[3] = 1.0f;
    vertex.uv[0] = uvs[i * 2];
    vertex.uv[1] = uvs[i * 2 + 1];
    lod.vertices.push_back(vertex);
    asset::SkinnedVertexExtra skin_vertex;
    skin_vertex.bone_indices[0] = static_cast<u8>(i);
    skin_vertex.bone_weights[0] = 255;
    lod.skinning.push_back(skin_vertex);
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), fabric.id});
  cloth_mesh = mesh.id.hash;
  if (!headless) {
    renderer.UploadMaterial(fabric);
    renderer.UploadMesh(mesh);
  }

  const asset::AssetId frame = AddMaterial("cloth_frame", {0.52f, 0.16f, 0.05f}, 0.3f, 0.65f);
  SpawnBox("cloth_top_bar", {0.84f, 0.055f, 0.055f}, origin + Vec3{0, 0.06f, 0.02f}, frame);
  SpawnBox("cloth_support_left", {0.045f, 2.7f, 0.045f},
           origin + Vec3{-0.84f, -2.7f, 0.02f}, frame);
  SpawnBox("cloth_support_right", {0.045f, 2.7f, 0.045f},
           origin + Vec3{0.84f, -2.7f, 0.02f}, frame);
}

void FeatureGym::Impl::CreateAnimation() {
  const Vec3 c = Info(Area::kAnimation).center;
  asset::Material biped_material;
  biped_material.id = asset::MakeAssetId("featuregym/material/biped");
  biped_material.base_color_factor[0] = 0.74f;
  biped_material.base_color_factor[1] = 0.48f;
  biped_material.base_color_factor[2] = 0.28f;
  biped_material.roughness_factor = 0.68f;
  asset::Mesh biped;
  asset::MakeSkinnedBiped(asset::MakeAssetId("featuregym/mesh/biped"), &skeleton, &biped);
  biped = WithMaterial(std::move(biped), biped_material.id);
  skin = biped.skin;
  biped_mesh = biped.id.hash;
  if (!headless) {
    renderer.UploadMaterial(biped_material);
    renderer.UploadMesh(biped);
  }
  graph = anim::BuildBipedLocomotionGraph(skeleton);
  rig.Bind(graph);
  feet.Bind(graph, 0.08f);
  remap = anim::BuildBoneRemap(skeleton, skin);
  pose.ResetToBind(skeleton);
  biped_position = c + Vec3{-7, 0.2f, -2};

  asset::Mesh morph = asset::MakeSphere(1.35f, 34, 48,
                                        asset::MakeAssetId("featuregym/mesh/morph"));
  morph = WithMaterial(std::move(morph),
                       AddMaterial("morph", {0.74f, 0.16f, 0.58f}, 0.32f));
  asset::MorphTarget target;
  target.name = "pulse";
  target.name_hash = asset::MakeAssetId(target.name).hash;
  target.position_deltas.reserve(morph.lods[0].vertices.size() * 3);
  for (const asset::Vertex& vertex : morph.lods[0].vertices) {
    const f32 wave = 0.26f * std::sin(vertex.position[1] * 6.0f);
    target.position_deltas.push_back(vertex.normal[0] * wave);
    target.position_deltas.push_back(vertex.normal[1] * wave);
    target.position_deltas.push_back(vertex.normal[2] * wave);
  }
  morph.morph_targets.push_back(std::move(target));
  morph_mesh = morph.id.hash;
  if (!headless) renderer.UploadMesh(morph);
  morph_transform = MakeTranslation(c + Vec3{6.5f, 1.7f, 1.0f});
  morph_previous = morph_transform;

  asset::AssetId step = AddMaterial("ik_step", {0.30f, 0.34f, 0.28f}, 0.94f);
  for (int i = 0; i < 6; ++i) {
    const f32 h = 0.06f + (i % 3) * 0.07f;
    SpawnBox("ik_step_" + std::to_string(i), {0.8f, h, 0.85f},
             c + Vec3{-6 + i * 2.1f, h + 0.1f, -1.5f}, step, true);
  }
}

void FeatureGym::Impl::CreatePost() {
  const Vec3 c = Info(Area::kPost).center;
  asset::Material glow;
  glow.id = asset::MakeAssetId("featuregym/material/post_glow");
  glow.base_color = emissive;
  glow.emissive = emissive;
  glow.base_color_factor[0] = 1.0f;
  glow.base_color_factor[1] = 0.24f;
  glow.base_color_factor[2] = 0.03f;
  glow.emissive_factor[0] = 14;
  glow.emissive_factor[1] = 3.5f;
  glow.emissive_factor[2] = 0.4f;
  glow.emissive_pulse[0] = 0.8f;
  glow.emissive_pulse[1] = 0.45f;
  glow.roughness_factor = 0.22f;
  if (!headless) renderer.UploadMaterial(glow);
  const asset::AssetId glow_mesh = UploadMesh(
      asset::MakeSphere(0.75f, 24, 32, asset::MakeAssetId("featuregym/mesh/post_glow")), glow.id);
  asset::AssetId black = AddMaterial("post_black", {0.008f, 0.008f, 0.012f}, 0.15f, 0.65f);
  asset::AssetId white = AddMaterial("post_white", {0.92f, 0.94f, 1.0f}, 0.2f);
  for (int i = 0; i < 7; ++i) {
    ecs::Entity moving = Spawn(i & 1 ? glow_mesh : UploadMesh(
        asset::MakeCube(0.65f, asset::MakeAssetId("featuregym/mesh/post_cube_" + std::to_string(i))),
        i & 2 ? white : black), c + Vec3{-9 + i * 3.0f, 1.1f, 1.0f + (i & 1) * 2.0f});
    world.Add(moving, GymMotion{.origin = c + Vec3{-9 + i * 3.0f, 1.1f, 1.0f + (i & 1) * 2.0f},
                                .axis = {0, 1, 0},
                                .speed = 0.7f + i * 0.16f,
                                .amplitude = 0.55f,
                                .phase = i * 0.6f});
  }
  for (int i = 0; i < 9; ++i) {
    const f32 value = static_cast<f32>(i) / 8.0f;
    asset::AssetId ramp = AddMaterial("post_ramp_" + std::to_string(i), {value, value, value}, 0.65f);
    SpawnBox("post_ramp_" + std::to_string(i), {0.6f, 1.8f, 0.35f},
             c + Vec3{-8 + i * 2.0f, 1.9f, -4.5f}, ramp);
  }
}

void FeatureGym::Impl::CreateCameraExhibit() {
  const Vec3 c = Info(Area::kPost).center;
  camera_base_mode = world.Create();
  scene::CameraMode base;
  base.view.position = c + Vec3{0, 6.4f, 13.5f};
  base.view.orientation = QuatBetween(Vec3{0, 0, -1}, Normalize(c + Vec3{0, 1.1f, 0} -
                                                               base.view.position));
  world.Add(camera_base_mode, base);

  camera_rig_mode = world.Create();
  world.Add(camera_rig_mode, scene::CameraAnchor{.position = c + Vec3{0, 0.4f, 0}});
  world.Add(camera_rig_mode, scene::CameraRigPose{});
  world.Add(camera_rig_mode, scene::CameraMode{});
  world.Add(camera_rig_mode, scene::CameraIntent{});
  world.Add(camera_rig_mode, scene::CameraOrbit{.yaw = 0.0f, .pitch = -0.12f});
  world.Add(camera_rig_mode,
            scene::CameraLocalOffset{.offset = {0, 1.8f, 0},
                                     .space = scene::CameraOffsetSpace::kAnchor});
  world.Add(camera_rig_mode,
            scene::CameraBoom{.distance = 8.5f, .shoulder_offset = 1.6f, .height_offset = 0.5f});
  world.Add(camera_rig_mode, scene::CameraFraming{.target_offset = {0, 1.2f, 0}});
  world.Add(camera_rig_mode,
            scene::CameraDamping{.position_half_life = 0.28f,
                                 .rotation_half_life = 0.18f,
                                 .lens_half_life = 0.25f});
  scene::CameraLensDrive lens;
  lens.lens.fov_y = 0.88f;
  lens.zoom_speed = 0.05f;
  world.Add(camera_rig_mode, lens);

  camera_output = world.Create();
  if (scene::InitializeCameraStack(world, camera_output, camera_base_mode) !=
      scene::CameraStackResult::kSuccess) {
    RX_WARN("feature gym: ECS camera stack unavailable");
  }
}

void FeatureGym::Impl::StartAudio() {
  if (!ctx.audio) return;
  const std::filesystem::path path = FeatureAssetPath("spatial_tone.wav");
  std::vector<u8> bytes = ReadBytes(path);
  if (bytes.empty()) {
    RX_WARN("feature gym: generated audio missing at {}", path.string());
    return;
  }
  std::unique_ptr<audio::Decoder> decoder =
      audio::OpenDecoder(ByteSpan(bytes.data(), bytes.size()), ".wav");
  audio::PlayParams params;
  params.loop = true;
  params.positional = true;
  params.position = Info(Area::kAnimation).center + Vec3{6, 1.5f, -5};
  params.gain = 0.18f;
  params.fade_in = 0.5f;
  audio_voice = ctx.audio->mixer().Play(std::move(decoder), params);
}

void FeatureGym::Impl::AddSimulation() {
  scheduler.AddSystem(ecs::Stage::kSim, "feature_gym_sim", [this](ecs::World& sim_world, f32 dt) {
    sim_time += dt;
    sim_world.Each<GymMotion, world::Transform>(
        [this](ecs::Entity, GymMotion& motion, world::Transform& transform) {
          const f32 angle = sim_time * motion.speed + motion.phase;
          transform.position[0] = motion.origin.x;
          transform.position[1] = motion.origin.y + std::sin(angle * 1.7f) * motion.amplitude;
          transform.position[2] = motion.origin.z;
          const Quat rotation = QuatFromAxisAngle(motion.axis, angle);
          transform.rotation[0] = rotation.x;
          transform.rotation[1] = rotation.y;
          transform.rotation[2] = rotation.z;
           transform.rotation[3] = rotation.w;
         });

#if RECREATION_HAS_NET
    sim_world.Each<BubbleAgent, world::Transform>(
        [dt](ecs::Entity, BubbleAgent& agent, world::Transform& transform) {
          agent.time += dt;
          transform.position[0] = agent.center.x + std::sin(agent.time * agent.rate_x) * agent.extent;
          transform.position[1] = agent.center.y;
          transform.position[2] = agent.center.z + std::cos(agent.time * agent.rate_z) * agent.extent;
        });
#endif

    if (strand_sim) {
      const f32 angle = sim_time * 0.85f;
      const Vec3 position = strand_center + Vec3{0.08f * std::sin(angle),
                                                 0.10f * std::sin(angle * 1.7f), 0};
      strand_transform = MakeTranslation(position) *
                         MakeFromQuat(QuatFromAxisAngle({0, 1, 0}, 0.7f * std::sin(angle)));
      physics.SetStrandGroomTransform(strand_sim, strand_transform, dt);
      physics.SetStrandGroomWind(
          strand_sim, {0.8f + 0.45f * std::sin(sim_time * 2.1f), 0.12f, -0.35f});
    }

    const f32 identity[4] = {0, 0, 0, 1};
    if (platform_body) {
      const Vec3 target = platform_origin + Vec3{std::sin(sim_time * 0.7f) * 3.5f, 0, 0};
      physics.MoveBodyKinematic(platform_body, target, identity, dt);
      if (world::Transform* transform = sim_world.Get<world::Transform>(platform_entity)) {
        transform->position[0] = target.x;
        transform->position[1] = target.y;
        transform->position[2] = target.z;
      }
    }
    if (character) {
      Vec3 position{};
      bool grounded = false;
      Vec3 ground_velocity{};
      world::Transform* transform = sim_world.Get<world::Transform>(character_entity);
      if (transform) {
        if (transform->position[0] > -22) character_direction = -1;
        if (transform->position[0] < -39) character_direction = 1;
      }
      const int cycle = static_cast<int>(sim_time / 5.0f);
      const bool jump = cycle != jump_cycle && std::fmod(sim_time, 5.0f) < dt * 1.5f;
      if (jump) jump_cycle = cycle;
      physics.MoveCharacter(character, {character_direction * 2.0f, 0, 0}, jump, dt, &position,
                            &grounded);
      (void)grounded;
      (void)ground_velocity;
      if (transform) {
        transform->position[0] = position.x;
        transform->position[1] = position.y;
        transform->position[2] = position.z;
      }
    }
    if (car) {
      if (active == Area::kPhysics) {
        const bool accelerating = physics_active_time < 1.0f;
        physics.DriveVehicle(car, accelerating ? 0.10f : 0.0f,
                             std::sin(physics_active_time * 0.35f) * 0.35f,
                             accelerating ? 0.0f : 0.65f, 0.0f);
      } else {
        physics.DriveVehicle(car, 0, 0, 1, 0);
      }
    }
    if (bike) {
      if (active == Area::kPhysics) {
        const bool accelerating = physics_active_time < 1.0f;
        physics.DriveVehicle(bike, accelerating ? 0.08f : 0.0f,
                             std::sin(physics_active_time * 0.42f) * 0.22f,
                             accelerating ? 0.0f : 0.55f, 0.0f);
      } else {
        physics.DriveVehicle(bike, 0, 0, 1, 0);
      }
    }
    physics_active_time = active == Area::kPhysics ? physics_active_time + dt : 0.0f;
  });
}

void FeatureGym::Impl::UpdateParticles(f32 dt, render::FrameView& view) {
  dt = std::min(dt, 0.05f);
  auto random = [&]() {
    particle_seed ^= particle_seed << 13;
    particle_seed ^= particle_seed >> 17;
    particle_seed ^= particle_seed << 5;
    return static_cast<f32>(particle_seed & 0xffffffu) / 16777216.0f;
  };
  for (size_t i = 0; i < particles.size();) {
    Particle& particle = particles[i];
    particle.life -= dt;
    if (particle.life <= 0) {
      particles[i] = particles.back();
      particles.pop_back();
      continue;
    }
    particle.velocity.y -= 1.2f * dt;
    particle.position += particle.velocity * dt;
    ++i;
  }
  particle_spawn += dt * 460.0f;
  const u32 count = static_cast<u32>(particle_spawn);
  particle_spawn -= count;
  for (u32 i = 0; i < count && particles.size() < 6000; ++i) {
    const f32 angle = random() * 2.0f * kPi;
    Particle particle;
    particle.position = particle_emitter;
    particle.velocity = {std::cos(angle) * (0.5f + random()), 2.8f + random() * 2.4f,
                         std::sin(angle) * (0.5f + random())};
    particle.max_life = particle.life = 1.4f + random() * 1.2f;
    particle.size = 0.08f + random() * 0.11f;
    particle.color = {0.25f + random() * 0.25f, 0.55f + random() * 0.4f, 1.0f};
    particles.push_back(particle);
  }
  for (const Particle& particle : particles) {
    const f32 life = particle.life / particle.max_life;
    render::ParticleInstance instance;
    instance.pos[0] = particle.position.x;
    instance.pos[1] = particle.position.y;
    instance.pos[2] = particle.position.z;
    instance.size = particle.size;
    instance.color[0] = particle.color.x;
    instance.color[1] = particle.color.y;
    instance.color[2] = particle.color.z;
    instance.color[3] = life * life;
    instance.prev_pos[0] = particle.position.x - particle.velocity.x * dt;
    instance.prev_pos[1] = particle.position.y - particle.velocity.y * dt;
    instance.prev_pos[2] = particle.position.z - particle.velocity.z * dt;
    view.particles.push_back(instance);
  }
}

void FeatureGym::Impl::EmitCloth(render::FrameView& view) {
  if (!cloth_mesh || !cloth_width ||
      !physics.GetClothPositions(cloth_id, cloth_positions.data(),
                                 static_cast<u32>(cloth_positions.size()))) {
    return;
  }

  std::fill(cloth_normals.begin(), cloth_normals.end(), Vec3{});
  cloth_lines.clear();
  for (size_t i = 0; i < cloth_indices.size(); i += 3) {
    const u32 a = cloth_indices[i];
    const u32 b = cloth_indices[i + 1];
    const u32 c = cloth_indices[i + 2];
    const Vec3 normal_value =
        Cross(cloth_positions[b] - cloth_positions[a], cloth_positions[c] - cloth_positions[a]);
    cloth_normals[a] += normal_value;
    cloth_normals[b] += normal_value;
    cloth_normals[c] += normal_value;
    cloth_lines.push_back({cloth_positions[a], cloth_positions[b], 0x84d5c5ff});
    cloth_lines.push_back({cloth_positions[b], cloth_positions[c], 0x84d5c5ff});
    cloth_lines.push_back({cloth_positions[c], cloth_positions[a], 0x84d5c5ff});
  }

  const i32 skin_offset = static_cast<i32>(view.bone_matrices.size());
  for (u32 i = 0; i < cloth_positions.size(); ++i) {
    Vec3 normal_value = Normalize(cloth_normals[i]);
    if (Length(normal_value) < 1.0e-5f) normal_value = {0, 0, 1};
    if (Dot(normal_value, view.camera.eye - cloth_positions[i]) < 0)
      normal_value = normal_value * -1.0f;
    const u32 x = i % cloth_width;
    const u32 left = x > 0 ? i - 1 : i;
    const u32 right = x + 1 < cloth_width ? i + 1 : i;
    Vec3 tangent = cloth_positions[right] - cloth_positions[left];
    tangent = Normalize(tangent - normal_value * Dot(tangent, normal_value));
    if (Length(tangent) < 1.0e-5f) tangent = Normalize(Cross({0, 1, 0}, normal_value));
    if (Length(tangent) < 1.0e-5f) tangent = {1, 0, 0};
    const Vec3 bitangent = Normalize(Cross(normal_value, tangent));

    Mat4 pose_matrix = Mat4::Identity();
    pose_matrix.m[0] = tangent.x;
    pose_matrix.m[1] = tangent.y;
    pose_matrix.m[2] = tangent.z;
    pose_matrix.m[4] = bitangent.x;
    pose_matrix.m[5] = bitangent.y;
    pose_matrix.m[6] = bitangent.z;
    pose_matrix.m[8] = normal_value.x;
    pose_matrix.m[9] = normal_value.y;
    pose_matrix.m[10] = normal_value.z;
    pose_matrix.m[12] = cloth_positions[i].x;
    pose_matrix.m[13] = cloth_positions[i].y;
    pose_matrix.m[14] = cloth_positions[i].z;
    view.bone_matrices.push_back(pose_matrix);
  }
  render::DrawItem draw;
  draw.mesh = cloth_mesh;
  draw.transform = Mat4::Identity();
  draw.prev_transform = draw.transform;
  draw.skin_offset = skin_offset;
  view.draws.push_back(draw);
  view.debug_lines_overlay =
      std::span<const render::DebugLine>(cloth_lines.data(), cloth_lines.size());
}

void FeatureGym::Impl::EmitAnimation(render::FrameView& view) {
  if (!graph.valid()) return;
  constexpr f32 dt = 1.0f / 60.0f;
  biped_time += dt;
  const f32 phase = std::fmod(biped_time, 8.0f);
  const f32 speed = phase < 1.0f ? 0.0f : phase < 3.0f ? 1.6f : 3.8f;
  rig.SetSpeed(speed);
  Vec3 root = rig.Update(dt, &pose, [&](const anim::RigPlayer::Event& event) {
    if (event.phase == anim::RigPlayer::Event::Phase::kPoint) {
      const f32 intensity = rig.SampleCurve(NameHash("FootstepIntensity"), 0.5f);
      (void)intensity;
    }
  });
  biped_position += root;
  if (biped_position.z > Info(Area::kAnimation).center.z + 7.0f)
    biped_position.z = Info(Area::kAnimation).center.z - 4.0f;
  const Mat4 actor = MakeTranslation(biped_position);
  const Mat4 inverse = Inverse(actor);
  feet.Apply(&pose, [&](const Vec3& origin, Vec3* hit, Vec3* hit_normal) {
    physics::PhysicsWorld::RayHit result;
    if (!physics.Raycast(TransformPoint(actor, origin), {0, -1, 0}, 2.0f, &result)) return false;
    *hit = TransformPoint(inverse, result.position);
    *hit_normal = Normalize(TransformDir(inverse, result.normal));
    return true;
  });
  anim::ComputeModelMatrices(skeleton, pose, &bone_model);
  anim::BuildSkinPalette(bone_model, skin, remap, &palette);
  const i32 offset = static_cast<i32>(view.bone_matrices.size());
  for (const Mat4& matrix : palette) view.bone_matrices.push_back(matrix);
  render::DrawItem draw;
  draw.mesh = biped_mesh;
  draw.transform = actor;
  draw.prev_transform = biped_previous_valid ? biped_previous : actor;
  draw.skin_offset = offset;
  view.draws.push_back(draw);
  biped_previous = actor;
  biped_previous_valid = true;

  render::DrawItem morph;
  morph.mesh = morph_mesh;
  morph.transform = morph_transform;
  morph.prev_transform = morph_previous;
  morph.morph_offset = static_cast<i32>(view.morph_weights.size());
  morph.morph_count = 1;
  view.morph_weights.push_back({0, std::sin(render_time * 1.7f) * 0.5f + 0.5f});
  view.draws.push_back(morph);
  morph_previous = morph_transform;
}

void FeatureGym::Impl::EmitVehicles(render::FrameView& view) {
  auto emit = [&](physics::VehicleId vehicle, u64 chassis, std::span<Mat4> wheel_previous,
                  std::span<bool> wheel_previous_valid, Mat4& chassis_previous,
                  bool& chassis_previous_valid) {
    if (!vehicle) return;
    Vec3 position;
    f32 rotation[4];
    if (!physics.GetVehicleTransform(vehicle, &position, rotation)) return;
    const Quat q{rotation[0], rotation[1], rotation[2], rotation[3]};
    render::DrawItem body;
    body.mesh = chassis;
    body.transform = MakeTranslation(position) * MakeFromQuat(q);
    body.prev_transform = chassis_previous_valid ? chassis_previous : body.transform;
    view.draws.push_back(body);
    chassis_previous = body.transform;
    chassis_previous_valid = true;
    for (u32 i = 0; i < wheel_previous.size(); ++i) {
      Vec3 wheel_position;
      f32 wheel_rotation[4];
      if (!physics.GetVehicleWheel(vehicle, i, &wheel_position, wheel_rotation)) continue;
      render::DrawItem wheel;
      wheel.mesh = wheel_mesh;
      wheel.transform = MakeTranslation(wheel_position) *
                         MakeFromQuat({wheel_rotation[0], wheel_rotation[1], wheel_rotation[2],
                                       wheel_rotation[3]});
      wheel.prev_transform = wheel_previous_valid[i] ? wheel_previous[i] : wheel.transform;
      view.draws.push_back(wheel);
      wheel_previous[i] = wheel.transform;
      wheel_previous_valid[i] = true;
    }
  };
  emit(car, car_mesh, car_wheel_previous, car_wheel_previous_valid, car_previous,
       car_previous_valid);
  emit(bike, bike_mesh, bike_wheel_previous, bike_wheel_previous_valid, bike_previous,
       bike_previous_valid);
}

void FeatureGym::Impl::UpdateInstanceExhibit() {
  if (active_mode != TourMode::kStreamedInstanceLifecycle || !prop_mesh || headless) return;
  if (active_mode_elapsed >= 0.65f && prop_lifecycle_phase == 0) {
    if (!renderer.UpdateInstanceGroup(prop_group, prop_updated_transforms))
      RX_WARN("feature gym: streamed instance group update failed");
    prop_lifecycle_phase = 1;
  }
  if (active_mode_elapsed >= 1.25f && prop_lifecycle_phase == 1) {
    renderer.DestroyInstanceGroup(prop_group);
    prop_group = {};
    prop_lifecycle_phase = 2;
  }
  if (active_mode_elapsed >= 1.8f && prop_lifecycle_phase == 2) {
    prop_group = renderer.CreateInstanceGroup(prop_mesh, prop_updated_transforms);
    if (!prop_group) RX_WARN("feature gym: streamed instance group recreation failed");
    prop_lifecycle_phase = 3;
  }
}

void FeatureGym::Impl::EmitStrandGroom() {
  if (!strand_groom) return;
  renderer.SetHairGroomTransform(strand_groom, strand_transform);
  if (!strand_sim || strand_positions.empty()) return;
  const u32 position_count = physics.StrandGroomPositionCount(strand_sim);
  if (position_count != strand_positions.size()) strand_positions.resize(position_count);
  if (position_count &&
      physics.GetStrandGroomPositions(strand_sim, strand_positions.data(), position_count)) {
    renderer.SetHairGroomPoints(strand_groom, strand_positions.data(), position_count);
  }
}

void FeatureGym::Impl::EmitNetworkBubbles(render::FrameView& view) {
#if RECREATION_HAS_NET
  if (active_mode != TourMode::kTransportFreeNetworkBubbles) return;
  bubble_map.Update(world, ++bubble_tick);
  world.Each<net::NetworkId, scene::Tint>(
      [this](ecs::Entity, net::NetworkId& id, scene::Tint& tint) {
        const u32 owner = bubble_map.OwnerOf(id.value);
        tint.rgb = owner == net::kNoPeer ? 0x555555 : net::PeerColor(owner);
      });
  if (bubble_viz) bubble_viz->Emit(view, bubble_map.bubbles());
#else
  (void)view;
#endif
}

void FeatureGym::Impl::EmitCameraExhibit(f32 dt, render::FrameView& view) {
  if (active_mode != TourMode::kEcsCameraStackRig || !camera_activation) return;
  if (scene::CameraOrbit* orbit = world.Get<scene::CameraOrbit>(camera_rig_mode))
    orbit->yaw = 0.22f * std::sin(active_mode_elapsed * 0.7f);
  if (scene::CameraAnchor* anchor = world.Get<scene::CameraAnchor>(camera_rig_mode)) {
    const Vec3 c = Info(Area::kPost).center;
    anchor->position = c + Vec3{std::sin(active_mode_elapsed * 0.6f) * 1.2f, 0.4f, 0};
    anchor->velocity = {std::cos(active_mode_elapsed * 0.6f) * 0.72f, 0, 0};
  }
  scene::BuildCameraRigs(world, dt);
  scene::PrepareCameraRigConstraints(world, dt);
  scene::ResolveCameraRigs(world, dt);
  scene::ResolveCameraStacks(world, dt);
  const scene::CameraOutput* output = world.Get<scene::CameraOutput>(camera_output);
  if (!output || !output->valid) return;
  view.camera.eye = output->view.position;
  view.camera.target = output->view.position + scene::CameraForward(output->view);
  view.camera.fov_y = output->view.lens.fov_y;
}

void FeatureGym::Impl::DrawPanel() {
#if defined(RECREATION_HAS_IMGUI)
  if (!ctx.config->feature_tour_shots.empty()) return;
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::SetNextWindowPos({18, 18}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);
  if (ImGui::Begin("RX Feature Gym", nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextUnformatted("SELF-CONTAINED ENGINE ACCEPTANCE WORLD");
    ImGui::Separator();
    ImGui::Text("active: %s", Info(active).name);
    ImGui::Text("draws: %u / %u visible", renderer.draws_visible(), renderer.draws_total());
    ImGui::Text("persistent instances: %zu in %zu group(s)", renderer.instance_count(),
                renderer.instance_group_count());
    ImGui::Text("gpu: %.2f ms   dynamic scale: %.2f", renderer.gpu_frame_ms(),
                renderer.dynamic_resolution_scale());
    ImGui::Separator();
    for (const AreaInfo& area : kAreas) {
      const bool selected = std::string_view(area.name) == Info(active).name;
      ImGui::TextColored(selected ? ImVec4{1.0f, 0.78f, 0.30f, 1.0f}
                                  : ImVec4{0.72f, 0.76f, 0.82f, 1.0f},
                         "%s %s", selected ? ">" : " ", area.name);
    }
    ImGui::Separator();
    ImGui::TextUnformatted("--feature-tour: deterministic camera + mode sweep");
    ImGui::TextUnformatted("F1: full RX render controls");
  }
  ImGui::End();
#endif
}

void FeatureGym::Impl::Emit(f32 dt, render::FrameView& view) {
  render_time += std::min(dt, 0.05f);
  if (!activations.empty()) {
    render::RenderSettings& settings = renderer.settings();
    if (active_mode == TourMode::kWeatherRain) {
      settings.precipitation = 0.88f;
      settings.precip_snow = false;
      settings.lightning = 0.55f;
      settings.aurora = false;
    } else if (active_mode == TourMode::kWeatherSnowAurora) {
      settings.precipitation = 0.88f;
      settings.precip_snow = true;
      settings.lightning = 0;
      settings.aurora = true;
    } else if (active_mode == TourMode::kInteriorFog) {
      settings.interior = true;
    }
  } else if (active == Area::kAtmosphere) {
    render::RenderSettings& settings = renderer.settings();
    settings.precipitation = 0.88f;
    settings.precip_snow = false;
    settings.lightning = 0.55f;
  }
  UpdateInstanceExhibit();
  EmitCameraExhibit(dt, view);
  if (activations.empty()) {
    Area nearest_area = Area::kOverview;
    f32 nearest_distance = 18.0f * 18.0f;
    if (view.camera.eye.y < 16.0f) {
      for (size_t i = 0; i < std::size(kAreas); ++i) {
        const Vec3 delta = view.camera.eye - kAreas[i].center;
        const f32 distance = delta.x * delta.x + delta.z * delta.z;
        if (distance < nearest_distance) {
          nearest_distance = distance;
          nearest_area = static_cast<Area>(i + 1);
        }
      }
    }
    if (nearest_area != active) ApplyArea(nearest_area);
  }
  if (active == Area::kPhysics) {
    EmitCloth(view);
    EmitVehicles(view);
  } else {
    car_previous_valid = false;
    bike_previous_valid = false;
    car_wheel_previous_valid.fill(false);
    bike_wheel_previous_valid.fill(false);
  }
  if (active == Area::kAnimation)
    EmitAnimation(view);
  else
    biped_previous_valid = false;
  if (active == Area::kEffects) {
    EmitStrandGroom();
    UpdateParticles(dt, view);
    view.gpu_particle_count = 24000;
    view.gpu_particle_emitter = Info(Area::kEffects).center + Vec3{-5.5f, 0.4f, 4.0f};
    view.gpu_particle_mode = 1;
    view.gpu_particle_radius = 0.55f;
    view.gpu_particle_intensity = 0.72f;
    view.fur_ball = true;
    view.fur_position = Info(Area::kEffects).center + Vec3{1.5f, 1.2f, 4.5f};
    view.oit = oit;
    view.gaussians = gaussians;
  }
  if (active == Area::kLighting) view.lights = lights;
  if (active == Area::kMaterials) view.decals = decals;
  EmitNetworkBubbles(view);
  view.debug_lines = std::span<const render::DebugLine>(lines.data(), lines.size());

  for (size_t i = 0; i < water_bodies.size(); ++i) {
    Vec3 position;
    f32 rotation[4];
    if (!physics.GetBodyTransform(water_bodies[i], &position, rotation)) continue;
    if (active != Area::kWater) {
      water_previous[i] = position;
      continue;
    }
    const Vec3 velocity = dt > 1e-5f ? (position - water_previous[i]) * (1.0f / dt) : Vec3{};
    water_previous[i] = position;
    render::WaterDisturbance disturbance;
    disturbance.position =
        {position.x, water_height + physics::GerstnerWaveHeight(position.x, position.z, sim_time),
         position.z};
    disturbance.radius = 0.8f;
    disturbance.ripple_strength = std::min(1.0f, Length(velocity) * 0.15f);
    disturbance.foam_amount = std::min(0.8f, Length(velocity) * 0.08f);
    disturbance.velocity_x = velocity.x;
    disturbance.velocity_z = velocity.z;
    disturbance.elongation = std::min(3.0f, Length(velocity) * 0.25f);
    view.water_disturbances.push_back(disturbance);
  }
  DrawPanel();
}

bool FeatureGym::Impl::BuildTour(ShowcaseCamera& camera) {
  if (!created) return false;
  activations.clear();
  applied_activation = std::numeric_limits<size_t>::max();
  auto add = [&](TourMode mode, Vec3 eye, Vec3 look, f32 travel, const char* label) {
    // A waypoint captures at the end of its segment. Keep its mode through that
    // boundary, then switch shortly after travel toward the next stop begins.
    const f32 delay = activations.empty() ? 0.0f : 0.1f;
    activations.push_back({camera.duration() + delay, mode});
    camera.Add({eye, look, travel, true, label});
  };
  camera.Add({{-47, 34, 44}, {0, 0, -12}, 0, false, {}});
  add(TourMode::kOverview, {-47, 34, 44}, {0, 0, -12}, 3.0f, "overview");

  auto district = [&](TourMode mode, f32 travel, const char* label) {
    const Area area = AreaForMode(mode);
    const Vec3 center = Info(area).center;
    add(mode, center + Vec3{0, 6.4f, 13.5f}, center + Vec3{0, 1.1f, 0}, travel, label);
  };
  district(TourMode::kMaterials, 4.0f, "materials");
  const Vec3 materials = Info(Area::kMaterials).center;
  add(TourMode::kSplitPbrPerSubmesh, materials + Vec3{-7, 3.8f, 6.5f},
      materials + Vec3{-7, 1.0f, -5}, 3.0f, "split_pbr_per_submesh_materials");
  add(TourMode::kTerrainSplatV2, materials + Vec3{4, 4.8f, 6.0f},
      materials + Vec3{4, 0.2f, -5}, 3.0f, "terrain_splat_v2");
  district(TourMode::kLightingRaster, 4.0f, "lighting_raster");
  district(TourMode::kLightingRaytraced, 3.0f, "lighting_raytraced");
  district(TourMode::kLightingRcgi, 3.0f, "lighting_rcgi_hw_or_sdf");
  district(TourMode::kGeometryScalability, 4.0f, "geometry_scalability");
  const Vec3 geometry = Info(Area::kGeometry).center;
  add(TourMode::kStreamedInstanceLifecycle, geometry + Vec3{0, 5.4f, 11.0f},
      geometry + Vec3{0, 0.8f, -2.5f}, 3.0f, "streamed_instance_lifecycle");
  add(TourMode::kProjectedVirtualGeometryAlbedo, geometry + Vec3{9, 4.2f, 6.5f},
      geometry + Vec3{0, 0.4f, -6}, 3.0f, "virtual_geometry_projected_albedo");
  district(TourMode::kWeatherRain, 4.0f, "weather_rain");
  district(TourMode::kWeatherSnowAurora, 3.0f, "weather_snow_aurora");
  district(TourMode::kWaterOcean, 4.0f, "water_ocean");
  const Vec3 water = Info(Area::kWater).center;
  add(TourMode::kGerstnerShorelineBuoyancy, water + Vec3{-5, 3.8f, 8.5f},
      water + Vec3{3.0f, 0.1f, -2.0f}, 3.5f, "gerstner_shoreline_buoyancy");
  district(TourMode::kEffects, 4.0f, "particles_transparency_hair");
  const Vec3 effects = Info(Area::kEffects).center;
  add(TourMode::kJoltProceduralStrandGroom, effects + Vec3{10, 4.5f, 10.5f},
      effects + Vec3{7, 2.0f, 4.5f}, 3.0f, "jolt_procedural_strand_groom");
  const Vec3 physics_center = Info(Area::kPhysics).center;
  add(TourMode::kPhysics, physics_center + Vec3{3.0f, 6.4f, 13.5f},
      physics_center + Vec3{3.0f, 1.1f, 0}, 4.0f, "physics_vehicles_character");
  add(TourMode::kTransportFreeNetworkBubbles, effects + Vec3{0, 14.0f, 14.5f},
      effects + Vec3{0, 0.5f, 0}, 3.5f, "transport_free_network_bubbles");
  district(TourMode::kAnimation, 4.0f, "animation_skin_morph_audio");
  district(TourMode::kEcsCameraStackRig, 4.0f, "ecs_camera_stack_rig");
  district(TourMode::kPostTaa, 4.0f, "post_taa");
  district(TourMode::kPostMsaa, 2.5f, "post_msaa");
  district(TourMode::kPostFsr3, 2.5f, "post_fsr3_fallback");
  district(TourMode::kPostDlss, 2.5f, "post_dlss_fallback");
  district(TourMode::kPostXess, 2.5f, "post_xess_fallback");
  district(TourMode::kPostTonemapGrade, 2.5f, "post_tonemap_grade");
  district(TourMode::kInteriorFog, 3.0f, "interior_fog");
  district(TourMode::kPathTraceReconstruction, 5.0f, "path_trace_reconstruction");
  return true;
}

void FeatureGym::Impl::ApplyArea(Area area) {
  active = area;
  render::RenderSettings& settings = renderer.settings();
  settings = baseline;
  settings.path_trace = false;
  settings.path_trace_recon = false;
  settings.interior = false;
  settings.precipitation = 0;
  settings.lightning = 0;
  settings.aurora = false;
  settings.fog = false;
  settings.upscaler = render::UpscalerKind::kNone;
  settings.aa_mode = render::AntiAliasingMode::kTaa;
  settings.render_scale = 1.0f;
  settings.dynamic_resolution = false;
  settings.frame_generation = false;
  settings.debug_view = render::DebugView::kOff;
  settings.rcgi = false;

  switch (area) {
    case Area::kOverview:
      settings.vrs = true;
      settings.async_compute = true;
      settings.gpu_culling = true;
      settings.gpu_occlusion = true;
      break;
    case Area::kMaterials:
      settings.sss = true;
      settings.rt_reflections = true;
      settings.ssr = true;
      settings.bloom = true;
      break;
    case Area::kLighting: {
      settings.rt_shadows = false;
      settings.rtao = false;
      settings.rt_reflections = false;
      settings.ddgi = false;
      settings.shadow_maps = true;
      settings.ssao = true;
      settings.ssr = true;
      settings.ssgi = true;
      settings.local_shadows = true;
      settings.froxel_fog = true;
      settings.froxel_density = 0.018f;
      settings.restir_di = false;
      break;
    }
    case Area::kGeometry:
      settings.gpu_culling = true;
      settings.gpu_occlusion = true;
      settings.distance_lod = true;
      settings.mesh_shader_lod = true;
      settings.vrs = true;
      settings.async_compute = true;
      settings.dynamic_resolution = ctx.config->feature_tour_shots.empty();
      settings.dynamic_target_ms = 16.6f;
      settings.texture_budget_mb = 96;
      break;
    case Area::kAtmosphere:
      settings.clouds = true;
      settings.cloud_coverage = 0.82f;
      settings.precipitation = 0.88f;
      settings.precip_snow = false;
      settings.lightning = 0.55f;
      settings.froxel_fog = true;
      settings.froxel_density = 0.022f;
      settings.aerial_perspective = 1.7f;
      break;
    case Area::kWater:
      settings.adaptive_water = true;
      settings.water_field = true;
      settings.water_interaction = true;
      settings.shore_wetting = true;
      settings.fft_ocean = true;
      settings.water_caustics = true;
      settings.water_reflections = true;
      settings.ssr = true;
      settings.water_rest_height = water_height;
      settings.shore_island[0] = Info(Area::kWater).center.x + 5.0f;
      settings.shore_island[1] = Info(Area::kWater).center.z - 2.0f;
      settings.shore_island[2] = 2.8f;
      settings.shore_island[3] = 0.75f;
      break;
    case Area::kEffects:
      settings.bloom = true;
      settings.bloom_intensity = 0.085f;
      settings.froxel_fog = true;
      settings.froxel_density = 0.012f;
      settings.motion_blur = true;
      break;
    case Area::kPhysics:
    case Area::kAnimation:
      settings.motion_blur = true;
      settings.distance_lod = true;
      settings.dof = false;
      break;
    case Area::kPost:
      settings.motion_blur = true;
      settings.dof = true;
      settings.dof_focus = 12.0f;
      settings.dof_aperture = 1.8f;
      settings.bloom = true;
      settings.bloom_intensity = 0.11f;
      settings.lens_flare = 0.12f;
      settings.chromatic_aberration = 2.0f;
      settings.vignette = 0.28f;
      settings.film_grain = 0.025f;
      break;
    case Area::kInterior:
      settings.interior = true;
      settings.interior_ambient = {0.06f, 0.045f, 0.08f};
      settings.interior_directional_color = {0.35f, 0.46f, 0.72f};
      settings.interior_directional_intensity = 1.4f;
      settings.interior_fog_near_color = {0.08f, 0.06f, 0.11f};
      settings.interior_fog_far_color = {0.02f, 0.03f, 0.06f};
      settings.interior_fog_near = 3.0f;
      settings.interior_fog_far = 42.0f;
      settings.interior_fog_power = 1.4f;
      settings.interior_fog_max = 0.82f;
      break;
    case Area::kPathTrace:
      settings.path_trace = true;
      settings.path_trace_reference = false;
      settings.path_trace_recon = true;
      settings.path_trace_restir = true;
      settings.path_trace_restir_di = true;
      settings.path_trace_rr = true;
      settings.path_trace_spp = 2;
      settings.path_trace_accum = 16;
      break;
  }
}

void FeatureGym::Impl::ApplyTourMode(TourMode mode) {
  if (camera_activation) {
    scene::ReleaseCameraMode(world, camera_activation, {.duration = 0});
    scene::ResolveCameraStacks(world, 0);
    camera_activation = {};
  }
  active_mode = mode;
  active_mode_elapsed = 0;
  ApplyArea(AreaForMode(mode));
  render::RenderSettings& settings = renderer.settings();
  switch (mode) {
    case TourMode::kLightingRaytraced:
      settings.rt_shadows = true;
      settings.rtao = true;
      settings.rt_reflections = true;
      settings.ddgi = true;
      settings.shadow_maps = false;
      settings.ssao = false;
      settings.ssgi = false;
      settings.restir_di = true;
      break;
    case TourMode::kLightingRcgi:
      settings.ibl = true;
      settings.rcgi = true;
      settings.rcgi_intensity = 1.2f;
      settings.ddgi = false;
      settings.ssgi = false;
      settings.shadow_maps = true;
      break;
    case TourMode::kStreamedInstanceLifecycle:
      if (!headless && prop_mesh) {
        if (prop_group) renderer.DestroyInstanceGroup(prop_group);
        prop_group = renderer.CreateInstanceGroup(prop_mesh, prop_transforms);
        if (!prop_group) RX_WARN("feature gym: streamed instance group reset failed");
      }
      prop_lifecycle_phase = 0;
      break;
    case TourMode::kWeatherSnowAurora:
      settings.precip_snow = true;
      settings.lightning = 0;
      settings.aurora = true;
      settings.sun_direction = Normalize(Vec3{0.1f, -0.04f, -0.99f});
      settings.sun_intensity = 0.18f;
      settings.ambient = 0.018f;
      break;
    case TourMode::kGerstnerShorelineBuoyancy:
      settings.fft_ocean = false;
      break;
    case TourMode::kEcsCameraStackRig: {
      if (scene::CameraMode* base = world.Get<scene::CameraMode>(camera_base_mode)) {
        base->view.position = ctx.camera->position();
        base->view.orientation =
            QuatBetween(Vec3{0, 0, -1}, Normalize(ctx.camera->target() - ctx.camera->position()));
      }
      scene::ResolveCameraStacks(world, 0);
      scene::BuildCameraRigs(world, 0);
      scene::PrepareCameraRigConstraints(world, 0);
      scene::ResolveCameraRigs(world, 0);
      const scene::CameraPushResult pushed = scene::PushCameraMode(
          world, camera_output, camera_rig_mode,
          {.duration = 1.1f,
           .easing = scene::CameraEasing::kSmootherStep,
           .discontinuity = scene::CameraDiscontinuityPolicy::kRetarget});
      if (pushed.result == scene::CameraStackResult::kSuccess)
        camera_activation = pushed.activation;
      else
        RX_WARN("feature gym: ECS camera mode activation failed");
      break;
    }
    case TourMode::kPostMsaa:
      settings.aa_mode = render::AntiAliasingMode::kMsaa;
      settings.msaa_samples = 4;
      break;
    case TourMode::kPostFsr3:
      settings.aa_mode = render::AntiAliasingMode::kUpscaler;
      settings.upscaler = render::UpscalerKind::kFsr3;
      settings.upscaler_quality = render::UpscalerQuality::kQuality;
      settings.sharpness = 0.2f;
      settings.frame_generation = true;
      break;
    case TourMode::kPostDlss:
      settings.aa_mode = render::AntiAliasingMode::kUpscaler;
      settings.upscaler = render::UpscalerKind::kDlss;
      settings.upscaler_quality = render::UpscalerQuality::kQuality;
      break;
    case TourMode::kPostXess:
      settings.aa_mode = render::AntiAliasingMode::kUpscaler;
      settings.upscaler = render::UpscalerKind::kXess;
      settings.upscaler_quality = render::UpscalerQuality::kQuality;
      break;
    case TourMode::kPostTonemapGrade:
      settings.tonemap = render::TonemapOperator::kAces;
      settings.color_grade = render::ColorGrade::kCinematic;
      settings.render_scale = 1.25f;
      break;
    case TourMode::kOverview:
    case TourMode::kMaterials:
    case TourMode::kSplitPbrPerSubmesh:
    case TourMode::kTerrainSplatV2:
    case TourMode::kLightingRaster:
    case TourMode::kGeometryScalability:
    case TourMode::kProjectedVirtualGeometryAlbedo:
    case TourMode::kWeatherRain:
    case TourMode::kWaterOcean:
    case TourMode::kEffects:
    case TourMode::kJoltProceduralStrandGroom:
    case TourMode::kPhysics:
    case TourMode::kTransportFreeNetworkBubbles:
    case TourMode::kAnimation:
    case TourMode::kPostTaa:
    case TourMode::kInteriorFog:
    case TourMode::kPathTraceReconstruction: break;
  }
}

void FeatureGym::Impl::SetTourTime(f32 seconds) {
  if (activations.empty()) return;
  size_t selected = 0;
  for (size_t i = 0; i < activations.size(); ++i) {
    if (activations[i].time <= seconds)
      selected = i;
    else
      break;
  }
  if (selected != applied_activation) {
    applied_activation = selected;
    ApplyTourMode(activations[selected].mode);
  }
  active_mode_elapsed = std::max(0.0f, seconds - activations[selected].time);
}

FeatureGym::FeatureGym(EngineContext& ctx) : impl_(std::make_unique<Impl>(ctx)) {}
FeatureGym::~FeatureGym() = default;

void FeatureGym::Create() { impl_->Create(); }
void FeatureGym::Emit(f32 dt, render::FrameView& view) { impl_->Emit(dt, view); }
bool FeatureGym::BuildTour(ShowcaseCamera& camera) { return impl_->BuildTour(camera); }
void FeatureGym::SetTourTime(f32 seconds) { impl_->SetTourTime(seconds); }
std::string_view FeatureGym::active_area() const { return Info(impl_->active).name; }

}  // namespace rx

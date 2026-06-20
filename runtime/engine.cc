#include "engine.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "core/log.h"
#include "core/math.h"
#include "world/components.h"

namespace rec {
namespace {

struct Spin {
  f32 angle = 0;
  f32 speed = 0.9f;
};

Mat4 TransformMatrix(const world::Transform& transform) {
  return MakeTranslation({transform.position[0], transform.position[1], transform.position[2]}) *
         MakeFromQuat(transform.rotation[0], transform.rotation[1], transform.rotation[2],
                      transform.rotation[3]) *
         MakeScale(transform.scale);
}

}  // namespace

bool Engine::Initialize(const EngineConfig& config) {
  config_ = config;
  jobs_ = std::make_unique<JobSystem>();

  if (!config_.headless) {
    window_ = Window::Create({});
    if (!renderer_.Initialize(config_.renderer, *window_)) return false;
  }

  if (!config_.headless) {
    if (!debug_ui_.Initialize(*window_, renderer_)) {
      REC_WARN("debug ui unavailable");
    }
  }

  if (physics_.Initialize()) {
    // A small cube every scene can throw around (F key).
    asset::Mesh cube = asset::MakeCube(0.25f, asset::MakeAssetId("builtin/physics_cube"));
    physics_cube_mesh_ = cube.id;
    if (!config_.headless) renderer_.UploadMesh(cube);
    scheduler_.AddSystem(ecs::Stage::kSim, "physics", [this](ecs::World& world, f32 dt) {
      physics_.Update(dt);
      for (const PhysicsEntity& body : physics_entities_) {
        world::Transform* transform = world.Get<world::Transform>(body.entity);
        if (!transform) continue;
        Vec3 position;
        f32 rotation[4];
        if (physics_.GetBodyTransform(body.body, &position, rotation)) {
          transform->position[0] = position.x;
          transform->position[1] = position.y;
          transform->position[2] = position.z;
          std::memcpy(transform->rotation, rotation, sizeof(rotation));
        }
      }
    });
  }

  if (!config_.gltf_path.empty()) {
    if (!LoadGltfScene()) return false;
  } else if (!config_.data_dir.empty()) {
    if (!LoadGameData()) return false;
  } else {
    CreateDemoScene();
  }

  if (!StartNetworking()) return false;

  scheduler_.AddSystem(ecs::Stage::kPostSim, "cell_streaming", [this](ecs::World& world, f32) {
    if (!streamer_) return;
    streamer_->Update(world, camera_.position());
  });

  return true;
}

bool Engine::StartNetworking() {
  net::SessionConfig net_config;
  net_config.port = config_.port;
  net_config.player_name = base::NameString(config_.player_name.c_str());
  net_config.max_clients = config_.max_clients;
  // Joining players replicate as cubes until there are real actor assets.
  net_config.player_mesh = asset::MakeAssetId("builtin/cube").hash;

  if (config_.host_server) {
    auto server = std::make_unique<net::ServerSession>(std::move(net_config));
    if (!server->Start()) return false;
    server_session_ = server.get();
    session_ = std::move(server);
  } else if (!config_.connect_address.empty()) {
    net_config.address = base::String(config_.connect_address.c_str());
    auto client = std::make_unique<net::ClientSession>(std::move(net_config));
    if (!client->Start()) return false;
    client_session_ = client.get();
    session_ = std::move(client);
  } else {
    return true;
  }

  scheduler_.AddSystem(ecs::Stage::kSim, "net", [this](ecs::World& world, f32 dt) {
    session_->Tick(world, dt);
  });
  if (client_session_) {
    // Remote transforms blend between snapshots. With a renderer that runs
    // per frame; headless clients smooth at the fixed step instead.
    const ecs::Stage stage =
        config_.headless ? ecs::Stage::kPostSim : ecs::Stage::kPreRender;
    scheduler_.AddSystem(stage, "net_interpolation", [](ecs::World& world, f32 dt) {
      net::TickInterpolation(world, dt);
    });
  }
  return true;
}

void Engine::CreateWaterDemoScene() {
  // Empty map with just water and a few reflectors: the fastest loop for
  // iterating water shading without streaming a game world.
  asset::Material water_material;
  water_material.id = asset::MakeAssetId("demo/water_material");
  water_material.base_color_factor[0] = 0.08f;
  water_material.base_color_factor[1] = 0.12f;
  water_material.base_color_factor[2] = 0.16f;
  water_material.base_color_factor[3] = 0.75f;
  water_material.metallic_factor = 0;
  water_material.roughness_factor = 0.05f;
  water_material.alpha_mode = asset::AlphaMode::kBlend;
  water_material.two_sided = true;
  water_material.is_water = true;

  asset::Mesh water;
  water.id = asset::MakeAssetId("demo/water");
  water.lods.emplace_back();
  asset::MeshLod& lod = water.lods[0];
  constexpr f32 kHalf = 120.0f;
  for (u32 i = 0; i < 4; ++i) {
    asset::Vertex v{};
    v.position[0] = (i & 1) ? kHalf : -kHalf;
    v.position[1] = 0;
    v.position[2] = (i & 2) ? kHalf : -kHalf;
    v.normal[1] = 1;
    v.tangent[0] = 1;
    v.tangent[3] = 1;
    v.uv[0] = v.position[0] / 8.0f;
    v.uv[1] = v.position[2] / 8.0f;
    v.color = 0xffffffff;
    lod.vertices.push_back(v);
  }
  for (u32 index : {0u, 1u, 2u, 1u, 3u, 2u}) lod.indices.push_back(index);
  asset::Submesh submesh;
  submesh.index_count = 6;
  submesh.material = water_material.id;
  lod.submeshes.push_back(submesh);
  water.bounds_radius = kHalf * 1.5f;

  asset::Mesh cube = asset::MakeCube(1.0f, asset::MakeAssetId("demo/cube"));
  asset::Mesh ground = asset::MakeCube(40.0f, asset::MakeAssetId("demo/ground"));
  if (!config_.headless) {
    renderer_.UploadMaterial(water_material);
    renderer_.UploadMesh(water);
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }

  // Sea floor far below, water sheet at origin, an island of cubes. The
  // cubes are rigid bodies light enough to float: jolt buoyancy keeps them
  // bobbing on the sheet.
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -48.0f, 0}});
  world_.Add(floor, world::Renderable{ground.id});
  ecs::Entity sheet = world_.Create();
  world_.Add(sheet, world::Transform{});
  world_.Add(sheet, world::Renderable{water.id});
  physics_.AddStaticBox({0, -48.0f, 0}, {40.0f, 40.0f, 40.0f});
  physics_.set_water_height([](const Vec3&, f32* height, Vec3* flow) {
    *height = 0.0f;
    if (flow) *flow = {};
    return true;
  });
  for (int i = 0; i < 6; ++i) {
    ecs::Entity block = world_.Create();
    f32 angle = static_cast<f32>(i) * 1.047f;
    Vec3 position{std::cos(angle) * 6.0f, 2.0f + (i % 3), std::sin(angle) * 6.0f};
    world_.Add(block, world::Transform{.position = {position.x, position.y, position.z}});
    world_.Add(block, world::Renderable{cube.id});
    physics::BodyId body = physics_.AddDynamicBox(position, {1.0f, 1.0f, 1.0f}, 400.0f, {});
    if (body) physics_entities_.push_back({body, block});
  }

  camera_.set_position({-14.0f, 3.0f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, -0.25f);
  REC_INFO("water demo scene");
}

void Engine::CreateDemoScene() {
  if (config_.demo_scene == "water") {
    CreateWaterDemoScene();
    return;
  }
  asset::Mesh cube = asset::MakeCube(0.7f, asset::MakeAssetId("builtin/cube"));
  asset::Mesh ground = asset::MakeCube(2.5f, asset::MakeAssetId("builtin/ground"));
  if (!config_.headless) {
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }

  if (!config_.connect_address.empty()) {
    // Clients get their world from server snapshots; the meshes above are
    // uploaded so replicated Renderables resolve. The demo input swings the
    // player cube in a circle to exercise the client-to-server path.
    scheduler_.AddSystem(ecs::Stage::kSim, "demo_input", [this](ecs::World&, f32 dt) {
      if (!client_session_ || !client_session_->joined()) return;
      demo_input_time_ += dt;
      net::PlayerInput input;
      input.move_x = std::cos(demo_input_time_ * 0.8f) * 0.5f;
      input.move_z = std::sin(demo_input_time_ * 0.8f) * 0.5f;
      input.yaw = demo_input_time_ * 0.8f;
      client_session_->SetInput(input);
    });
    REC_INFO("no game data given, joining as demo client");
    return;
  }

  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{});
  world_.Add(entity, world::Renderable{cube.id});
  world_.Add(entity, Spin{});

  // Ground under the cube so raytraced shadows have something to land on.
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -3.6f, 0}});
  world_.Add(floor, world::Renderable{ground.id});

  if (config_.host_server) {
    world_.Add(entity, net::AllocateNetworkId());
    world_.Add(floor, net::AllocateNetworkId());
  }

  scheduler_.AddSystem(ecs::Stage::kSim, "demo_spin", [](ecs::World& world, f32 dt) {
    world.Each<Spin, world::Transform>([dt](ecs::Entity, Spin& spin, world::Transform& t) {
      spin.angle += spin.speed * dt;
      t.rotation[1] = std::sin(spin.angle * 0.5f);
      t.rotation[3] = std::cos(spin.angle * 0.5f);
    });
  });
  REC_INFO("no game data given, spinning a cube instead");
}

bool Engine::LoadGameData() {
  game_ = config_.game != bethesda::Game::kUnknown
              ? config_.game
              : bethesda::GameProfile::DetectFromDataDir(config_.data_dir);
  if (game_ == bethesda::Game::kUnknown) {
    REC_ERROR("could not detect a supported game in {}", config_.data_dir);
    return false;
  }
  const auto& profile = bethesda::GameProfile::For(game_);
  REC_INFO("detected {}", profile.name);

  MountArchives();
  // Loose files mount last so they win over archives.
  vfs_.Mount(asset::MakeLooseFileProvider(config_.data_dir));

  assets_ = std::make_unique<asset::AssetDatabase>(vfs_);
  bethesda::RegisterConverters(*assets_, profile);

  auto order = bethesda::LoadOrder::FromPluginsTxt(config_.plugins_txt, profile);
  if (!records_.LoadAll(config_.data_dir, order, profile)) return false;
  REC_INFO("{} plugins, {} records", order.plugins().size(), records_.record_count());

  streamer_ = std::make_unique<world::CellStreamer>(records_, *assets_);
  if (physics_.initialized()) {
    streamer_->set_physics(&physics_);
    physics_.set_water_height([this](const Vec3& position, f32* height, Vec3* flow) {
      return streamer_->WaterHeightAt(position, height, flow);
    });
  }
  world::CellStreamer::Settings settings;
  settings.grass_density = config_.grass_density;
  streamer_->Configure(settings);
  if (!config_.headless) {
    world::CellStreamer::Uploads uploads;
    uploads.mesh = [this](const asset::Mesh& mesh) { return renderer_.UploadMesh(mesh); };
    uploads.texture = [this](const asset::Texture& texture) {
      return renderer_.UploadTexture(texture);
    };
    uploads.material = [this](const asset::Material& material) {
      return renderer_.UploadMaterial(material);
    };
    streamer_->SetUploads(std::move(uploads));
  }

  if (!config_.interior.empty()) return LoadInterior();
  if (!streamer_->SelectWorldspace(profile.exterior_worldspace)) return false;

  // Drop the camera a bit above the terrain at the middle of the start cell.
  constexpr f32 kUnitsToMeters = 0.01428f;
  constexpr f32 kCellSize = 4096.0f;
  f32 beth_x = (static_cast<f32>(config_.start_cell_x) + 0.5f) * kCellSize;
  f32 beth_y = (static_cast<f32>(config_.start_cell_y) + 0.5f) * kCellSize;
  Vec3 start{beth_x * kUnitsToMeters, 0.0f, -beth_y * kUnitsToMeters};
  f32 ground = 0;
  if (streamer_->GroundHeight(start.x, start.z, &ground)) {
    start.y = ground + 10.0f;  // a little above the terrain for a view
  } else {
    REC_WARN("no terrain at start cell {},{}", config_.start_cell_x, config_.start_cell_y);
  }
  camera_.set_position(start);
  camera_.set_yaw_pitch(0.0f, -0.1f);
  camera_.speed = 30.0f;
  REC_INFO("camera start: cell {},{} at ({:.1f}, {:.1f}, {:.1f})", config_.start_cell_x,
           config_.start_cell_y, start.x, start.y, start.z);
  return true;
}

bool Engine::LoadInterior() {
  bethesda::GlobalFormId cell_id;
  if (config_.interior.starts_with("0x") || config_.interior.starts_with("0X")) {
    // Load order form id: top byte is the plugin index for full plugins.
    u32 raw = static_cast<u32>(std::stoul(config_.interior.substr(2), nullptr, 16));
    cell_id = {static_cast<u16>(raw >> 24), raw & 0xffffff};
  } else {
    cell_id = records_.FindInteriorCell(config_.interior);
  }
  if (cell_id.plugin == 0xffff) {
    REC_ERROR("interior cell not found: {}", config_.interior);
    return false;
  }

  Vec3 start{};
  if (!streamer_->LoadInterior(world_, cell_id, &start)) return false;
  camera_.set_position(start);
  camera_.set_yaw_pitch(0.0f, 0.0f);
  camera_.speed = 5.0f;
  REC_INFO("camera start: interior {} at ({:.1f}, {:.1f}, {:.1f})", config_.interior, start.x,
           start.y, start.z);
  return true;
}

void Engine::MountArchives() {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(config_.data_dir, ec)) {
    std::string path = entry.path().string();
    // TODO: archive order should follow plugin order plus the ini resource
    // lists, alphabetical is a placeholder.
    if (auto provider = bethesda::OpenArchive(path)) vfs_.Mount(std::move(provider));
  }
}

bool Engine::LoadGltfScene() {
  asset::GltfScene scene;
  if (!asset::LoadGltfScene(config_.gltf_path, &scene)) return false;

  if (!config_.headless) {
    for (const asset::Texture& texture : scene.textures) {
      if (texture.id) renderer_.UploadTexture(texture);
    }
    for (const asset::Material& material : scene.materials) renderer_.UploadMaterial(material);
    for (const asset::Mesh& mesh : scene.meshes) renderer_.UploadMesh(mesh);
  }

  for (const asset::GltfScene::Instance& instance : scene.instances) {
    ecs::Entity entity = world_.Create();
    world::Transform transform;
    transform.position[0] = instance.position.x;
    transform.position[1] = instance.position.y;
    transform.position[2] = instance.position.z;
    std::memcpy(transform.rotation, instance.rotation, sizeof(transform.rotation));
    transform.scale = instance.scale;
    world_.Add(entity, transform);
    world_.Add(entity, world::Renderable{scene.meshes[instance.mesh_index].id});
  }

  // Sponza-friendly start: inside the atrium looking down the long axis.
  camera_.set_position({-7.0f, 1.7f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, 0.0f);
  camera_.speed = 4.0f;
  return true;
}

void Engine::UpdateCamera(f32 frame_delta) {
  if (!window_) return;
  const InputState& input = window_->input();

  bool allow_mouse = !debug_ui_.wants_mouse() || camera_.looking();
  bool allow_keyboard = !debug_ui_.wants_keyboard();
  camera_.Update(input, allow_mouse, allow_keyboard, frame_delta);
  window_->SetRelativeMouseMode(camera_.looking());

  if (input.key_pressed(Key::kF1) && !debug_ui_.wants_keyboard()) debug_ui_.ToggleVisible();
  if (input.key_pressed(Key::kF) && !debug_ui_.wants_keyboard()) ThrowPhysicsCube();
  if (input.key_pressed(Key::kEscape) && !debug_ui_.wants_keyboard()) RequestQuit();
}

void Engine::ThrowPhysicsCube() {
  if (!physics_.initialized() || !physics_cube_mesh_) return;
  Vec3 forward = camera_.forward();
  Vec3 origin = camera_.position() + forward * 0.8f;
  // Wood-ish density: heavy enough to splash, light enough to float.
  physics::BodyId body = physics_.AddDynamicBox(origin, {0.25f, 0.25f, 0.25f}, 350.0f,
                                                forward * 14.0f);
  if (!body) return;
  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{.position = {origin.x, origin.y, origin.z}});
  world_.Add(entity, world::Renderable{physics_cube_mesh_});
  physics_entities_.push_back({body, entity});
}

int Engine::Run() {
  while (!quit_.load(std::memory_order_relaxed)) {
    if (window_ && !window_->PumpEvents()) break;

    int steps = timer_.Tick();
    f32 dt = static_cast<f32>(timer_.fixed_step());
    for (int i = 0; i < steps; ++i) {
      scheduler_.RunStage(ecs::Stage::kPreSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kPostSim, world_, dt);
    }

    if (!config_.headless) {
      f32 frame_delta = static_cast<f32>(timer_.frame_delta());
      debug_ui_.BeginFrame();
      UpdateCamera(frame_delta);
      scheduler_.RunStage(ecs::Stage::kPreRender, world_, frame_delta);

      render::FrameView view;
      view.camera.eye = camera_.position();
      view.camera.target = camera_.target();
      view.frame_delta_seconds = frame_delta;
      // Rebuilt every frame so destroyed entities drop out on their own.
      base::UnorderedMap<u64, Mat4> transforms;
      world_.Each<world::Transform, world::Renderable>(
          [&](ecs::Entity entity, world::Transform& transform, world::Renderable& renderable) {
            u64 key = static_cast<u64>(entity.generation) << 32 | entity.index;
            Mat4 current = TransformMatrix(transform);
            const Mat4* prev = prev_transforms_.find(key);
            view.draws.push_back({renderable.mesh.hash, current, prev ? *prev : current});
            transforms.insert(key, current);
          });
      prev_transforms_ = std::move(transforms);
      debug_ui_.Build(renderer_, camera_, frame_delta, &view);
      renderer_.RenderFrame(view);
    } else {
      // No vsync to pace the loop; yield between fixed steps instead of
      // spinning a core.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return 0;
}

void Engine::Shutdown() {
  if (!config_.headless) {
    renderer_.WaitIdle();
    debug_ui_.Shutdown();
    renderer_.Shutdown();
  }
  jobs_->WaitIdle();
}

}  // namespace rec

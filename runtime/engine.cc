#include "engine.h"

#include <cmath>
#include <filesystem>

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

  if (!config_.data_dir.empty()) {
    if (!LoadGameData()) return false;
  } else {
    CreateDemoScene();
  }

  if (config_.host_server) {
    session_ = std::make_unique<net::ServerSession>(config_.port, replication_);
  } else if (!config_.connect_address.empty()) {
    session_ = std::make_unique<net::ClientSession>(
        net::Endpoint{config_.connect_address, config_.port}, replication_);
  }

  scheduler_.AddSystem(ecs::Stage::kSim, "net", [this](ecs::World& world, f32 dt) {
    if (session_) session_->Tick(world, dt);
  });
  scheduler_.AddSystem(ecs::Stage::kPostSim, "cell_streaming", [this](ecs::World& world, f32) {
    if (!streamer_) return;
    f32 player_position[3] = {0, 0, 0};  // TODO: from the player entity
    streamer_->Update(world, player_position);
  });

  return true;
}

void Engine::CreateDemoScene() {
  asset::Mesh cube = asset::MakeCube(0.7f, asset::MakeAssetId("builtin/cube"));
  renderer_.UploadMesh(cube);

  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{});
  world_.Add(entity, world::Renderable{cube.id});
  world_.Add(entity, Spin{});

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

int Engine::Run() {
  while (!quit_) {
    if (window_ && !window_->PumpEvents()) break;

    int steps = timer_.Tick();
    f32 dt = static_cast<f32>(timer_.fixed_step());
    for (int i = 0; i < steps; ++i) {
      scheduler_.RunStage(ecs::Stage::kPreSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kPostSim, world_, dt);
    }

    if (!config_.headless) {
      scheduler_.RunStage(ecs::Stage::kPreRender, world_,
                          static_cast<f32>(timer_.frame_delta()));

      render::FrameView view;
      view.camera.eye = {2.4f, 1.8f, 2.4f};
      // Rebuilt every frame so destroyed entities drop out on their own.
      std::unordered_map<u64, Mat4> transforms;
      world_.Each<world::Transform, world::Renderable>(
          [&](ecs::Entity entity, world::Transform& transform, world::Renderable& renderable) {
            u64 key = static_cast<u64>(entity.generation) << 32 | entity.index;
            Mat4 current = TransformMatrix(transform);
            auto prev = prev_transforms_.find(key);
            view.draws.push_back({renderable.mesh.hash, current,
                                  prev != prev_transforms_.end() ? prev->second : current});
            transforms.emplace(key, current);
          });
      prev_transforms_ = std::move(transforms);
      renderer_.RenderFrame(view);
    }
  }
  return 0;
}

void Engine::Shutdown() {
  if (!config_.headless) renderer_.Shutdown();
  jobs_->WaitIdle();
}

}  // namespace rec

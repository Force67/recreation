#ifndef RECREATION_RUNTIME_ENGINE_H_
#define RECREATION_RUNTIME_ENGINE_H_

#include <memory>
#include <string>

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "bethesda/game_profile.h"
#include "bethesda/load_order.h"
#include "core/frame_timer.h"
#include "core/job_system.h"
#include "core/window.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#include "net/session.h"
#include "render/renderer.h"
#include "world/cell_streaming.h"

namespace rec {

struct EngineConfig {
  std::string data_dir;
  std::string plugins_txt;
  bethesda::Game game = bethesda::Game::kUnknown;  // kUnknown = autodetect
  render::RendererDesc renderer;
  bool headless = false;
  bool host_server = false;
  u16 port = 29700;
  std::string connect_address;
};

class Engine {
 public:
  bool Initialize(const EngineConfig& config);
  int Run();
  void Shutdown();

 private:
  bool LoadGameData();
  void MountArchives();
  void CreateDemoScene();

  EngineConfig config_;
  bethesda::Game game_ = bethesda::Game::kUnknown;

  std::unique_ptr<Window> window_;
  std::unique_ptr<JobSystem> jobs_;
  FrameTimer timer_;

  ecs::World world_;
  ecs::Scheduler scheduler_;

  asset::Vfs vfs_;
  std::unique_ptr<asset::AssetDatabase> assets_;
  bethesda::RecordStore records_;
  std::unique_ptr<world::CellStreamer> streamer_;

  render::Renderer renderer_;
  net::ReplicationRegistry replication_;
  std::unique_ptr<net::Session> session_;

  bool quit_ = false;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_ENGINE_H_

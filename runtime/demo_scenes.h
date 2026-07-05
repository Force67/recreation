#ifndef RECREATION_RUNTIME_DEMO_SCENES_H_
#define RECREATION_RUNTIME_DEMO_SCENES_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "engine_context.h"
#include "render/core/renderer.h"

namespace rec {

class ActorSystem;

// Builds the engine's standalone demo / bring-up scenes (selected by
// --demo-scene) and owns the CPU-side effect state (the particle fountain, the
// gaussian splats, the oit/point-light instance lists) those scenes set up.
// EmitToView feeds that state into the per-frame render view.
class DemoScenes {
 public:
  DemoScenes(EngineContext& ctx, ActorSystem* actors);

  // Dispatches on config.demo_scene; the default spins a cube + test biped.
  void CreateDemoScene();
  // Appends the live demo effects (particles, gaussians, oit, lights, fur, gpu
  // particles) into this frame's render view.
  void EmitToView(f32 dt, render::FrameView& view);

 private:
  void CreateWaterDemoScene();
  void CreateMaterialDemoScene();
  void CreateGaussianDemoScene();
  void CreateLodDemoScene();
  void CreateCornellDemoScene();
  void CreateGpuParticleDemoScene();
  void CreateFurDemoScene();
  void CreateAutoLodDemoScene();
  void CreateMaterialXDemoScene();
  void CreateOitDemoScene();
  void CreateOcclusionDemoScene();
  void CreateMeshletDemoScene();
  void CreatePointLightDemoScene();
  void UpdateParticles(f32 dt, render::FrameView& view);
  void CreateFireDemoScene();
  void CreateBrickDemoScene();
  void CreateVirtualTextureDemoScene();
  void CreateVirtualGeometryDemoScene();
  void CreateStrandHairDemoScene();
  void CreateImposterDemoScene();
  void CreateSssDemoScene();

  struct DemoParticle {
    Vec3 position;
    Vec3 velocity;
    f32 life = 0;
    f32 max_life = 1;
    f32 size = 0.1f;
    Vec3 color;
  };

  EngineContext& ctx_;
  ActorSystem* actors_;
  ecs::World& world_;
  ecs::Scheduler& scheduler_;
  render::Renderer& renderer_;
  FlyCamera& camera_;
  physics::PhysicsWorld& physics_;
  const EngineConfig& config_;

  bool particles_enabled_ = false;
  Vec3 particle_emitter_{0, 0, 0};
  u32 gpu_particle_count_ = 0;  // > 0 selects the gpu-simulated fountain
  Vec3 gpu_particle_emitter_{0, 0, 0};
  base::Vector<render::Decal> demo_decals_;
  u32 gpu_particle_mode_ = 0;         // 1 = fire (buoyant flames + embers)
  f32 gpu_particle_radius_ = 0.3f;
  f32 gpu_particle_intensity_ = 1.0f;
  f32 fire_time_ = 0.0f;  // drives the campfire light flicker
  bool fur_ball_ = false;
  Vec3 fur_position_{0, 0, 0};
  base::Vector<render::WboitInstance> oit_instances_;
  base::Vector<DemoParticle> demo_particles_;
  base::Vector<render::GaussianInstance> demo_gaussians_;
  base::Vector<render::PointLight> demo_lights_;
  u32 particle_seed_ = 0x9e3779b9u;
  f32 particle_spawn_accum_ = 0;
  f32 demo_input_time_ = 0;
  base::Vector<u32> hair_grooms_;      // strand-hair demo groom handles
  u32 hair_orbit_groom_ = 0;           // the groom driven on a slow orbit
  Vec3 hair_orbit_center_{0, 0, 0};
  f32 hair_time_ = 0;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_DEMO_SCENES_H_

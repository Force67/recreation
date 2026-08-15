#ifndef RECREATION_RUNTIME_DEMO_DEMO_SCENES_H_
#define RECREATION_RUNTIME_DEMO_DEMO_SCENES_H_

#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "core/math.h"
#include "render/core/renderer.h"
#include "runtime/app/engine_context.h"
#include "runtime/character/face.h"
#include "runtime/magic/spell_vfx.h"

namespace rx {

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
  // A row of assembled, morphed, Loop-subdivided FaceGen heads from real NPCs.
  // Requires loaded game data (records + vfs); routed here from LoadGameData when
  // --demo faces is passed with a --data-dir.
  void CreateFacesDemoScene();
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
  void CreateSpellDemoScene();

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
  u32 gpu_particle_mode_ = 0;  // 1 = fire (buoyant flames + embers)
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

  // FaceGen head demo state: the shared builder (stable address; the faces cache
  // pointers into it) and one editable FaceState per assembled head.
  base::UniquePointer<FaceBuilder> face_builder_;
  base::Vector<FaceState> faces_;

  // Spell demo: one channelled effect per archetype, plus periodic impacts.
  magic::SpellVfx spells_;
  base::Vector<Vec3> spell_pedestals_;
  bool spell_demo_ = false;
  f32 spell_time_ = 0;
  f32 spell_impact_accum_ = 0;
  u32 spell_impact_next_ = 0;

  base::Vector<u32> hair_grooms_;  // strand-hair demo groom handles
  u32 hair_orbit_groom_ = 0;       // the groom driven on a slow orbit
  Vec3 hair_orbit_center_{0, 0, 0};
  f32 hair_time_ = 0;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_DEMO_DEMO_SCENES_H_

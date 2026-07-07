#ifndef RECREATION_RENDER_DYNAMIC_RESOLUTION_H_
#define RECREATION_RENDER_DYNAMIC_RESOLUTION_H_

#include "core/types.h"

namespace rec::render {

// Dynamic resolution scaling: trades internal render resolution for GPU frame
// time. Every scale change goes through the renderer's full resize path
// (device idle, transient pool flush, TAA history reset), so the controller is
// deliberately coarse: quantized steps, hysteresis on a smoothed signal, and a
// cooldown after each change instead of a continuous loop. Frame-count based
// throughout, so behaviour is deterministic under REC_FIXED_DT.
class DynamicResolution {
 public:
  struct Settings {
    f32 target_ms = 16.6f;  // gpu frame budget the controller holds
    f32 min_scale = 0.5f;   // floor on scale() (per axis)
  };

  void Configure(const Settings& settings) { settings_ = settings; }

  // Feed one frame's resolved gpu time. Returns true when scale() changed;
  // the caller owns the resize that makes it effective.
  bool Update(f32 gpu_ms);

  // Back to native. Called whenever the controller is inactive so a reduced
  // scale never leaks into a mode that doesn't run it.
  void Reset();

  f32 scale() const { return scale_; }

 private:
  static constexpr f32 kStep = 0.05f;       // scale quantum (per axis)
  static constexpr f32 kOvershoot = 1.08f;  // ema/target ratio that arms a drop
  static constexpr f32 kHeadroom = 0.92f;   // predicted/target ratio to raise
  static constexpr u32 kDownFrames = 8;     // over-budget frames before dropping
  static constexpr u32 kUpFrames = 180;     // headroom frames before raising
  static constexpr u32 kCooldownFrames = 60;  // settle time after any change

  bool Apply(f32 next);

  Settings settings_;
  f32 scale_ = 1.0f;
  f32 ema_ms_ = 0.0f;
  u32 over_ = 0;
  u32 under_ = 0;
  u32 cooldown_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_DYNAMIC_RESOLUTION_H_

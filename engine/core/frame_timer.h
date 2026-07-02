#ifndef RECREATION_CORE_FRAME_TIMER_H_
#define RECREATION_CORE_FRAME_TIMER_H_

#include <chrono>

#include "core/types.h"

namespace rec {

// Fixed timestep simulation clock with a variable render delta.
class FrameTimer {
 public:
  explicit FrameTimer(f64 fixed_step = 1.0 / 60.0);

  // Call once per frame. Returns the number of fixed steps to simulate.
  int Tick();

  // Lockstep mode: every Tick advances by exactly this delta instead of the
  // wall clock, making time-driven animation a pure function of the frame
  // index (golden-image captures, replay tests). 0 restores real time.
  void set_fixed_delta(f64 seconds) { fixed_delta_ = seconds; }

  f64 fixed_step() const { return fixed_step_; }
  f64 frame_delta() const { return frame_delta_; }
  f64 interpolation_alpha() const { return accumulator_ / fixed_step_; }
  u64 frame_index() const { return frame_index_; }

 private:
  using Clock = std::chrono::steady_clock;

  f64 fixed_step_;
  f64 fixed_delta_ = 0.0;  // > 0 = lockstep
  f64 accumulator_ = 0.0;
  f64 frame_delta_ = 0.0;
  u64 frame_index_ = 0;
  Clock::time_point last_;
};

}  // namespace rec

#endif  // RECREATION_CORE_FRAME_TIMER_H_

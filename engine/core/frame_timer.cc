#include "core/frame_timer.h"

#include <algorithm>

namespace rec {

FrameTimer::FrameTimer(f64 fixed_step) : fixed_step_(fixed_step), last_(Clock::now()) {}

int FrameTimer::Tick() {
  auto now = Clock::now();
  frame_delta_ = fixed_delta_ > 0.0 ? fixed_delta_
                                    : std::chrono::duration<f64>(now - last_).count();
  last_ = now;
  ++frame_index_;

  // Clamp so a debugger pause does not produce a spiral of death.
  accumulator_ += std::min(frame_delta_, 0.25);
  int steps = 0;
  while (accumulator_ >= fixed_step_) {
    accumulator_ -= fixed_step_;
    ++steps;
  }
  return steps;
}

}  // namespace rec

#include "script/papyrus/fiber_scheduler.h"

#include <utility>

namespace rx::script::papyrus {

bool FiberScheduler::Run(std::function<void()> body, f64 real_now, f64 game_now) {
  // A fresh top-level activation starts from the baseline, not a parked fiber's
  // in-flight context (which stays with that fiber until it resumes).
  if (reset_context_) reset_context_();
  auto fiber = std::make_unique<Fiber>(std::move(body));
  fiber->Resume();
  if (fiber->done()) return false;
  Park(std::move(fiber), real_now, game_now);
  return true;
}

void FiberScheduler::Park(std::unique_ptr<Fiber> fiber, f64 real_now, f64 game_now) {
  const LatentRequest req = take_request_();
  Parked p;
  p.real_due = req.real_seconds >= 0 ? real_now + req.real_seconds : -1.0;
  p.game_due = req.game_days >= 0 ? game_now + req.game_days : -1.0;
  if (capture_context_) p.restore = capture_context_();
  p.fiber = std::move(fiber);
  parked_.push_back(std::move(p));
}

void FiberScheduler::Advance(f64 real_now, f64 game_now) {
  // Move the due activations out before resuming: a resumed fiber may park a new
  // one, and resuming must not run on a fiber that re-parks itself this pass.
  std::vector<Parked> due;
  for (auto it = parked_.begin(); it != parked_.end();) {
    const bool ready = (it->real_due >= 0 && real_now >= it->real_due) ||
                       (it->game_due >= 0 && game_now >= it->game_due);
    if (ready) {
      due.push_back(std::move(*it));
      it = parked_.erase(it);
    } else {
      ++it;
    }
  }
  for (Parked& p : due) {
    if (p.restore) p.restore();
    p.fiber->Resume();
    if (!p.fiber->done()) Park(std::move(p.fiber), real_now, game_now);
  }
}

}  // namespace rx::script::papyrus

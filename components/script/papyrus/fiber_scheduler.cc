#include <base/containers/vector.h>
#include <base/functional/function.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>

#include "components/script/papyrus/fiber_scheduler.h"

namespace rx::script::papyrus {

bool FiberScheduler::Run(base::Function<void()> body, f64 real_now, f64 game_now) {
  // A fresh top-level activation starts from the baseline, not a parked fiber's
  // in-flight context (which stays with that fiber until it resumes).
  if (reset_context_) reset_context_();
  auto fiber = base::MakeUnique<Fiber>(base::move(body));
  fiber->Resume();
  if (fiber->done()) return false;
  Park(base::move(fiber), real_now, game_now);
  return true;
}

void FiberScheduler::Park(base::UniquePointer<Fiber> fiber, f64 real_now, f64 game_now) {
  const LatentRequest req = take_request_();
  Parked p;
  p.real_due = req.real_seconds >= 0 ? real_now + req.real_seconds : -1.0;
  p.game_due = req.game_days >= 0 ? game_now + req.game_days : -1.0;
  if (capture_context_) p.restore = capture_context_();
  p.fiber = base::move(fiber);
  parked_.push_back(base::move(p));
}

void FiberScheduler::Advance(f64 real_now, f64 game_now) {
  // Move the due activations out before resuming: a resumed fiber may park a new
  // one, and resuming must not run on a fiber that re-parks itself this pass.
  base::Vector<Parked> due;
  for (mem_size i = 0; i < parked_.size();) {
    const Parked& parked = parked_[i];
    const bool ready = (parked.real_due >= 0 && real_now >= parked.real_due) ||
                       (parked.game_due >= 0 && game_now >= parked.game_due);
    if (ready) {
      due.push_back(base::move(parked_[i]));
      parked_.erase(i);
    } else {
      ++i;
    }
  }
  for (Parked& p : due) {
    if (p.restore) p.restore();
    p.fiber->Resume();
    if (!p.fiber->done()) Park(base::move(p.fiber), real_now, game_now);
  }
}

}  // namespace rx::script::papyrus

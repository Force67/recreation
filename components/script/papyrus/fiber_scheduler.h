#ifndef RECREATION_SCRIPT_PAPYRUS_FIBER_SCHEDULER_H_
#define RECREATION_SCRIPT_PAPYRUS_FIBER_SCHEDULER_H_

#include <base/containers/vector.h>
#include <base/functional/function.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>

#include "components/script/papyrus/fiber.h"
#include "core/types.h"

namespace rx::script::papyrus {

// Runs script activations on fibers so a latent native (Utility.Wait) can suspend
// the whole call chain and resume it later. An activation that does not wait runs
// to completion immediately; one that suspends is parked with a deadline and
// resumed by Advance once enough real or game time has passed.
//
// Single-threaded: every method runs on the guest thread, the only place the VM
// executes. `take_request` returns the suspending activation's wait (the VM's
// TakeLatentRequest), queried right after a fiber yields.
//
// In-session only: a parked fiber holds its continuation on a live stack, which
// cannot be serialized. A save captured mid-Wait therefore loses the pending wait.
// TODO: to persist parked activations across a save, snapshot each suspended
// activation's frame stack (script, function, ip, locals per frame) as data and
// resume it via a stackless path. Needs a savegame system first; the timer and LOS
// registrations in PapyrusGuest are already serializable and would persist first.
class FiberScheduler {
 public:
  explicit FiberScheduler(base::Function<LatentRequest()> take_request)
      : take_request_(base::move(take_request)) {}

  // Optional per-activation context that must stay fiber-local across a suspend
  // (the bindings' quest provenance and fragment-recursion depth). `reset`
  // establishes a fresh baseline when a new top-level activation starts, so it does
  // not inherit a parked fiber's in-flight values. `capture`, called when a fiber
  // yields, returns a closure that restores that fiber's context; the scheduler
  // runs it just before resuming the fiber. Without hooks, no context is tracked.
  void set_context_hooks(base::Function<void()> reset,
                         base::Function<base::Function<void()>()> capture) {
    reset_context_ = base::move(reset);
    capture_context_ = base::move(capture);
  }

  // Runs `body` on a fresh fiber at the given clock. Returns true if it suspended
  // and was parked, false if it ran to completion.
  bool Run(base::Function<void()> body, f64 real_now, f64 game_now);

  // Resumes every parked activation whose deadline has passed; a resumed one that
  // suspends again is re-parked with a fresh deadline.
  void Advance(f64 real_now, f64 game_now);

  size_t parked() const { return parked_.size(); }

 private:
  struct Parked {
    base::UniquePointer<Fiber> fiber;
    f64 real_due;                    // resume when real_now >= this; <0 if not a real-time wait
    f64 game_due;                    // resume when game_now >= this; <0 if not a game-time wait
    base::Function<void()> restore;  // re-establishes this fiber's context before resume
  };

  // Reads the just-yielded fiber's wait request and stores it as a deadline.
  void Park(base::UniquePointer<Fiber> fiber, f64 real_now, f64 game_now);

  base::Function<LatentRequest()> take_request_;
  base::Function<void()> reset_context_;
  base::Function<base::Function<void()>()> capture_context_;
  base::Vector<Parked> parked_;
};

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_FIBER_SCHEDULER_H_

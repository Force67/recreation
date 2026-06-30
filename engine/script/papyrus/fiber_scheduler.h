#ifndef RECREATION_SCRIPT_PAPYRUS_FIBER_SCHEDULER_H_
#define RECREATION_SCRIPT_PAPYRUS_FIBER_SCHEDULER_H_

#include <functional>
#include <memory>
#include <vector>

#include "core/types.h"
#include "script/papyrus/fiber.h"

namespace rec::script::papyrus {

// Runs script activations on fibers so a latent native (Utility.Wait) can suspend
// the whole call chain and resume it later. An activation that does not wait runs
// to completion immediately; one that suspends is parked with a deadline and
// resumed by Advance once enough real or game time has passed.
//
// Single-threaded: every method runs on the guest thread, the only place the VM
// executes. `take_request` returns the suspending activation's wait (the VM's
// TakeLatentRequest), queried right after a fiber yields.
class FiberScheduler {
 public:
  explicit FiberScheduler(std::function<LatentRequest()> take_request)
      : take_request_(std::move(take_request)) {}

  // Optional per-activation context that must survive a suspend. `capture` reads it
  // when a fiber yields; `restore` writes it back just before that fiber resumes,
  // so a value clobbered by another fiber running in between (the bindings' quest
  // provenance) is correct again on resume. Without hooks, no context is tracked.
  void set_context_hooks(std::function<u64()> capture, std::function<void(u64)> restore) {
    capture_context_ = std::move(capture);
    restore_context_ = std::move(restore);
  }

  // Runs `body` on a fresh fiber at the given clock. Returns true if it suspended
  // and was parked, false if it ran to completion.
  bool Run(std::function<void()> body, f64 real_now, f64 game_now);

  // Resumes every parked activation whose deadline has passed; a resumed one that
  // suspends again is re-parked with a fresh deadline.
  void Advance(f64 real_now, f64 game_now);

  size_t parked() const { return parked_.size(); }

 private:
  struct Parked {
    std::unique_ptr<Fiber> fiber;
    f64 real_due;  // resume when real_now >= this; <0 if not a real-time wait
    f64 game_due;  // resume when game_now >= this; <0 if not a game-time wait
    u64 context;   // captured at suspend, restored before resume
  };

  // Reads the just-yielded fiber's wait request and stores it as a deadline.
  void Park(std::unique_ptr<Fiber> fiber, f64 real_now, f64 game_now);

  std::function<LatentRequest()> take_request_;
  std::function<u64()> capture_context_;
  std::function<void(u64)> restore_context_;
  std::vector<Parked> parked_;
};

}  // namespace rec::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_FIBER_SCHEDULER_H_

#ifndef RECREATION_SCRIPT_PAPYRUS_FIBER_H_
#define RECREATION_SCRIPT_PAPYRUS_FIBER_H_

#include <cstddef>
#include <functional>

#include "core/types.h"

// The coroutine backend (minicoro) type, kept out of this header.
struct mco_coro;

namespace rx::script::papyrus {

// A latent native's request to suspend its activation: how long until it resumes.
// real_seconds >= 0 is a real-time wait (Utility.Wait); game_days >= 0 is an
// in-game-time wait (Utility.WaitGameTime). A negative field means "not this
// kind"; normally exactly one is set.
struct LatentRequest {
  f64 real_seconds = -1.0;
  f64 game_days = -1.0;
};

// A cooperative execution context with its own stack. It runs `entry` when first
// resumed and can suspend mid-call (YieldCurrent), switching back to its resumer
// with the full C++ call chain frozen on its stack, then continue from exactly
// there on the next Resume.
//
// This is the primitive behind latent Papyrus natives (Utility.Wait): a script
// activation runs on a fiber, so Wait can yield without unwinding the interpreter.
// Backed by minicoro, which switches stacks on the same OS thread (asm backend,
// or Win32 fibers / ucontext fallback) across Linux/macOS/Windows/Android. Same
// thread matters: the Papyrus VM is confined to its guest thread, so the
// activation must not migrate to another thread while suspended.
class Fiber {
 public:
  explicit Fiber(std::function<void()> entry, std::size_t stack_bytes = 256 * 1024);
  ~Fiber();

  Fiber(const Fiber&) = delete;
  Fiber& operator=(const Fiber&) = delete;

  // Switch into the fiber from the current (resumer) context. Returns when the
  // fiber yields or its entry returns. Must not be called once done().
  void Resume();
  bool done() const { return done_; }

  // Suspends the fiber currently running on this thread, switching back to its
  // resumer; returns when that fiber is Resumed again. Returns false (a no-op) if
  // no fiber is running on this thread.
  static bool YieldCurrent();

  // The fiber running on this thread, or null at the top level.
  static Fiber* current();

 private:
  struct Context;  // holds the two ucontext_t, kept out of the header

  static void Trampoline(mco_coro* co);
  void Yield();

  std::function<void()> entry_;
  std::size_t stack_bytes_;
  bool done_ = false;
  bool started_ = false;
  bool aborting_ = false;  // tearing down a still-suspended fiber: unwind on resume
  Context* ctx_;
};

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_FIBER_H_

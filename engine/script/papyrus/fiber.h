#ifndef RECREATION_SCRIPT_PAPYRUS_FIBER_H_
#define RECREATION_SCRIPT_PAPYRUS_FIBER_H_

#include <cstddef>
#include <functional>
#include <vector>

namespace rec::script::papyrus {

// A cooperative execution context with its own stack. It runs `entry` when first
// resumed and can suspend mid-call (YieldCurrent), switching back to its resumer
// with the full C++ call chain frozen on its stack, then continue from exactly
// there on the next Resume.
//
// This is the primitive behind latent Papyrus natives (Utility.Wait): a script
// activation runs on a fiber, so Wait can yield without unwinding the interpreter.
// POSIX ucontext backs it (Linux/macOS/Android); a Windows fiber backend would
// slot in behind the same interface.
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

  static void Trampoline();
  void Yield();

  std::function<void()> entry_;
  std::vector<char> stack_;
  bool done_ = false;
  bool started_ = false;
  Context* ctx_;
};

}  // namespace rec::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_FIBER_H_

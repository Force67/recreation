#include "script/papyrus/fiber.h"

#include <cassert>

#include "minicoro.h"  // API only; the implementation is in minicoro_impl.cc

namespace rec::script::papyrus {
namespace {
// The fiber currently executing on this thread. Maintained across every context
// switch so YieldCurrent and the trampoline always find the right one.
thread_local Fiber* t_current = nullptr;

// Thrown at the suspend point when a still-suspended fiber is destroyed, so its
// frozen C++ call chain unwinds (running every local's destructor) instead of
// being abandoned with the stack. Not a std::exception, so ordinary handlers do
// not swallow it; any catch(...) on the latent path must rethrow it. It is thrown
// and caught entirely within the coroutine's own stack, so it never crosses the
// context switch.
struct FiberUnwind {};
}  // namespace

struct Fiber::Context {
  mco_coro* co = nullptr;
};

Fiber::Fiber(std::function<void()> entry, std::size_t stack_bytes)
    : entry_(std::move(entry)), stack_bytes_(stack_bytes), ctx_(new Context) {
  mco_desc desc = mco_desc_init(&Fiber::Trampoline, stack_bytes_);
  desc.user_data = this;
  const mco_result r = mco_create(&ctx_->co, &desc);
  assert(r == MCO_SUCCESS && "mco_create failed");
  (void)r;
}

Fiber::~Fiber() {
  // A fiber abandoned mid-suspend would leak everything its frozen stack owns, so
  // resume it once in unwind mode: the suspend point throws FiberUnwind, the stack
  // unwinds through every frame's destructors, and the entry returns.
  if (started_ && !done_) {
    aborting_ = true;
    Resume();
  }
  if (ctx_->co) mco_destroy(ctx_->co);  // now dead (ran to end) or never started
  delete ctx_;
}

void Fiber::Trampoline(mco_coro* co) {
  auto* self = static_cast<Fiber*>(mco_get_user_data(co));
  try {
    self->entry_();
  } catch (const FiberUnwind&) {
    // Torn down while suspended: the stack has unwound cleanly, nothing more to do.
  }
  self->done_ = true;
  // Returning ends the coroutine (mco marks it dead) and switches back to the
  // resumer, so control lands after the mco_resume that started this fiber.
}

void Fiber::Resume() {
  assert(!done_ && "Resume called on a finished fiber");
  Fiber* prev = t_current;
  t_current = this;
  started_ = true;
  // Starts the coroutine on first call, resumes it thereafter; returns here when
  // the fiber yields or its entry returns.
  const mco_result r = mco_resume(ctx_->co);
  assert(r == MCO_SUCCESS && "mco_resume failed");
  (void)r;
  t_current = prev;
}

void Fiber::Yield() {
  mco_yield(ctx_->co);
  if (aborting_) throw FiberUnwind{};  // resumed only to tear down: unwind the stack
}

bool Fiber::YieldCurrent() {
  if (!t_current) return false;
  t_current->Yield();
  return true;
}

Fiber* Fiber::current() { return t_current; }

}  // namespace rec::script::papyrus

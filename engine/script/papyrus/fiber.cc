#include "script/papyrus/fiber.h"

#include <ucontext.h>

#include <cassert>

namespace rec::script::papyrus {
namespace {
// The fiber currently executing on this thread. Maintained across every context
// switch so YieldCurrent and the trampoline always find the right one.
thread_local Fiber* t_current = nullptr;
}  // namespace

struct Fiber::Context {
  ucontext_t fiber{};
  ucontext_t resumer{};
};

Fiber::Fiber(std::function<void()> entry, std::size_t stack_bytes)
    : entry_(std::move(entry)), stack_(stack_bytes), ctx_(new Context) {}

Fiber::~Fiber() { delete ctx_; }

void Fiber::Trampoline() {
  Fiber* self = t_current;
  self->entry_();
  self->done_ = true;
  // Returning falls through to uc_link (the resumer set in Resume), so control
  // lands back after the swapcontext that started this fiber.
}

void Fiber::Resume() {
  assert(!done_ && "Resume called on a finished fiber");
  Fiber* prev = t_current;
  t_current = this;
  if (!started_) {
    started_ = true;
    getcontext(&ctx_->fiber);
    ctx_->fiber.uc_stack.ss_sp = stack_.data();
    ctx_->fiber.uc_stack.ss_size = stack_.size();
    ctx_->fiber.uc_link = &ctx_->resumer;
    makecontext(&ctx_->fiber, &Fiber::Trampoline, 0);
  }
  // Save the resumer and switch in; returns here when the fiber yields or ends.
  swapcontext(&ctx_->resumer, &ctx_->fiber);
  t_current = prev;
}

void Fiber::Yield() { swapcontext(&ctx_->fiber, &ctx_->resumer); }

bool Fiber::YieldCurrent() {
  if (!t_current) return false;
  t_current->Yield();
  return true;
}

Fiber* Fiber::current() { return t_current; }

}  // namespace rec::script::papyrus

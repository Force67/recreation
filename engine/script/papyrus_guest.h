#ifndef RECREATION_SCRIPT_PAPYRUS_GUEST_H_
#define RECREATION_SCRIPT_PAPYRUS_GUEST_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bethesda/game_profile.h"
#include "core/types.h"
#include "script/papyrus/native.h"
#include "script/papyrus/value.h"
#include "script/papyrus/vm.h"

namespace rec::script {

// The Papyrus guest world. It owns one papyrus::VirtualMachine and runs it on a
// single dedicated thread, fully isolated from the host: the engine and the
// .NET host reach it only through this thread-safe facade, and the VM is never
// touched from any other thread. That is what keeps the guest compartmentalized
// (the task's "papyrus vm runs on its own thread").
//
// The class is per game: a Skyrim guest pairs the shared VM core with the
// Skyrim native table. A Fallout guest would pair the same core with a Fallout
// table, so adding a dialect later does not touch the VM.
class PapyrusGuest {
 public:
  explicit PapyrusGuest(bethesda::Game game);
  ~PapyrusGuest();

  PapyrusGuest(const PapyrusGuest&) = delete;
  PapyrusGuest& operator=(const PapyrusGuest&) = delete;

  void Start();
  void Stop();
  bool running() const { return running_; }

  // Runs fn on the guest thread. Submit is fire-and-forget; SubmitFor returns a
  // future carrying fn's result. Both are safe to call from any thread and both
  // accept move-only callables (events carry move-only argument vectors).
  void Submit(std::move_only_function<void(papyrus::VirtualMachine&)> fn);
  template <typename Fn>
  auto SubmitFor(Fn fn) -> std::future<decltype(fn(std::declval<papyrus::VirtualMachine&>()))>;

  // Thread-safe convenience wrappers over Submit/SubmitFor.
  std::future<std::string> LoadScript(std::vector<u8> pex);
  std::future<papyrus::ObjectRef> CreateInstance(std::string type);
  void RaiseEvent(papyrus::ObjectRef target, std::string event,
                  std::vector<papyrus::Value> args = {});
  // Advances the guest clock by dt seconds and fires any due update events.
  void Tick(f32 dt);

  // The native table, exposed so a game binding can register its surface before
  // Start(). After Start() the table must not be mutated from outside.
  papyrus::NativeRegistry& natives() { return natives_; }

 private:
  struct ScheduledUpdate {
    papyrus::ObjectRef target;
    f64 due;
    f64 interval;  // 0 = one-shot
  };

  void ThreadMain();
  void BindEngineNatives();          // timers + debug, captured on this guest
  void ScheduleUpdate(papyrus::ObjectRef target, f64 due, f64 interval);
  void CancelUpdate(papyrus::ObjectRef target);
  void AdvanceUpdates(f64 dt);       // guest thread only

  bethesda::Game game_;
  papyrus::NativeRegistry natives_;
  papyrus::VirtualMachine vm_;

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::move_only_function<void(papyrus::VirtualMachine&)>> queue_;
  bool stop_ = false;
  bool running_ = false;

  // Touched only on the guest thread.
  f64 clock_ = 0;
  std::vector<ScheduledUpdate> updates_;
};

template <typename Fn>
auto PapyrusGuest::SubmitFor(Fn fn)
    -> std::future<decltype(fn(std::declval<papyrus::VirtualMachine&>()))> {
  using R = decltype(fn(std::declval<papyrus::VirtualMachine&>()));
  auto task = std::make_shared<std::packaged_task<R(papyrus::VirtualMachine&)>>(std::move(fn));
  std::future<R> future = task->get_future();
  Submit([task](papyrus::VirtualMachine& vm) { (*task)(vm); });
  return future;
}

}  // namespace rec::script

#endif  // RECREATION_SCRIPT_PAPYRUS_GUEST_H_

#ifndef RECREATION_SCRIPT_PAPYRUS_GUEST_H_
#define RECREATION_SCRIPT_PAPYRUS_GUEST_H_

#include <array>
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
#include "core/move_only_function.h"
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
  void Submit(MoveOnlyFunction<void(papyrus::VirtualMachine&)> fn);
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

  // Resolves a bare reference's type for `obj as Type` casts. Set before Start()
  // alongside the native table (see VirtualMachine::set_type_resolver).
  void set_type_resolver(std::function<bool(papyrus::ObjectRef, const std::string&)> r) {
    vm_.set_type_resolver(std::move(r));
  }

  // Where Debug.Notification messages go (a HUD toast in the runtime). Without a
  // handler they fall back to the trace log. Set it on the guest thread (Submit)
  // so the native, which also runs there, never races the assignment.
  void set_on_notification(std::function<void(const std::string&)> fn) {
    on_notification_ = std::move(fn);
  }

  // Where the multiplayer platform's Hud.* / Net.* calls go (the runtime's
  // PlatformHud, drained onto the on-screen HUD). Game-agnostic: these natives are
  // bound for every guest, so a server's UI works in any universe. Without a
  // handler the calls are inert. Set and read on the guest thread.
  void set_on_platform_hud(
      std::function<void(const std::string&, const std::string&,
                         const std::vector<papyrus::Value>&)>
          fn) {
    on_platform_hud_ = std::move(fn);
  }

  // Supplies the local player's world position (engine space) to the Net.LocalPos*
  // natives, so a mod can place blips and objects relative to the player. Set by
  // the runtime; read on the guest thread.
  void set_local_pos_provider(std::function<std::array<f32, 3>()> fn) {
    local_pos_provider_ = std::move(fn);
  }

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
  std::deque<MoveOnlyFunction<void(papyrus::VirtualMachine&)>> queue_;
  bool stop_ = false;
  bool running_ = false;

  // Touched only on the guest thread.
  f64 clock_ = 0;
  std::vector<ScheduledUpdate> updates_;

  // Set once on the guest thread (see set_on_notification); read by the
  // Debug.Notification native, also on the guest thread.
  std::function<void(const std::string&)> on_notification_;

  // Set once on the guest thread (see set_on_platform_hud); read by the platform
  // HUD/Net natives, also on the guest thread.
  std::function<void(const std::string&, const std::string&,
                     const std::vector<papyrus::Value>&)>
      on_platform_hud_;

  // Set once on the guest thread (see set_local_pos_provider); read by the
  // Net.LocalPos* natives, also on the guest thread.
  std::function<std::array<f32, 3>()> local_pos_provider_;
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

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
#include "script/papyrus/fiber_scheduler.h"
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

  // True while called from the guest thread itself (i.e. from inside a job the
  // guest is running). Lets a re-entrant caller skip the cross-thread hop.
  bool OnGuestThread() const;

  // Runs fn against the VM and returns its result synchronously. When the caller
  // is already on the guest thread this invokes fn directly (no queue, no future,
  // no context switch); otherwise it posts to the guest thread and blocks on the
  // result, exactly like SubmitFor(fn).get(). This is the boundary the .NET host
  // calls through: with the managed tick driven on the guest thread, every
  // C#->VM call takes the direct path instead of a per-call round-trip. Calling
  // SubmitFor().get() from the guest thread would self-deadlock; Dispatch cannot.
  template <typename Fn>
  auto Dispatch(Fn fn) -> decltype(fn(std::declval<papyrus::VirtualMachine&>())) {
    if (OnGuestThread()) return fn(vm_);
    return SubmitFor(std::move(fn)).get();
  }

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

  // Expands a form reference into the quest aliases it currently fills, so an
  // event RaiseEvent dispatches (OnActivate, OnTriggerEnter/Leave, OnInit) also
  // reaches the alias scripts, the way the bindings' native-raised events
  // (OnDeath, OnHit) already do via RaiseFormAndAliasEvent. The runtime wires
  // this to the bindings' alias index; unset means no alias routing. Set on and
  // called from the guest thread.
  void set_alias_resolver(std::function<std::vector<papyrus::ObjectRef>(papyrus::ObjectRef)> fn) {
    alias_resolver_ = std::move(fn);
  }

  // Where Debug.Notification messages go (a HUD toast in the runtime). Without a
  // handler they fall back to the trace log. Set it on the guest thread (Submit)
  // so the native, which also runs there, never races the assignment.
  void set_on_notification(std::function<void(const std::string&)> fn) {
    on_notification_ = std::move(fn);
  }

  // Where engine-reaching Debug.* commands go (quit, screenshot, the global dev
  // toggles): a verb plus a string argument. The runtime marshals these onto the
  // main loop and performs the real action. Without a handler they are inert. Set
  // on the guest thread; read by the natives, also on the guest thread.
  void set_on_debug_command(std::function<void(const std::string&, const std::string&)> fn) {
    on_debug_command_ = std::move(fn);
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
  // Runs one script activation on a fiber, so a latent native (Utility.Wait) can
  // suspend it; it parks until its wait elapses. Call on the guest thread. Used by
  // the firing sites here and by the bindings' fiber runner (engine-triggered
  // stage fragments).
  void RunScript(std::function<void()> body);

  // Wires the per-activation context the scheduler keeps fiber-local across a
  // suspend (the bindings' quest provenance and fragment depth). Set by the
  // runtime; see FiberScheduler.
  void set_fiber_context_hooks(std::function<void()> reset,
                               std::function<std::function<void()>()> capture) {
    fiber_sched_.set_context_hooks(std::move(reset), std::move(capture));
  }

  void set_local_pos_provider(std::function<std::array<f32, 3>()> fn) {
    local_pos_provider_ = std::move(fn);
  }

  // Supplies the current game time in days (WorldClock::game_days), driving the
  // RegisterForUpdateGameTime timers and their OnUpdateGameTime callbacks. Set by
  // the runtime; read on the guest thread.
  void set_game_time_provider(std::function<f64()> fn) { game_time_provider_ = std::move(fn); }

  // Answers whether a viewer ref has line of sight to a target ref, driving the
  // RegisterForLOS watches and their OnGainLOS/OnLostLOS callbacks. Set by the
  // runtime to the binding's HasLos; read on the guest thread.
  void set_los_provider(std::function<bool(u64, u64)> fn) { los_provider_ = std::move(fn); }

 private:
  struct ScheduledUpdate {
    papyrus::ObjectRef target;
    f64 due;
    f64 interval;  // 0 = one-shot
  };

  // A line-of-sight watch: when `viewer`'s sight of `target` changes, the event
  // fires on `registrant`. mode selects which transitions matter and whether the
  // watch is one-shot.
  enum class LosMode : u8 { kBoth, kSingleGain, kSingleLost };
  struct LosWatch {
    papyrus::ObjectRef registrant;
    papyrus::ObjectRef viewer;
    papyrus::ObjectRef target;
    LosMode mode;
    bool has_los;  // last observed state, so only transitions fire
  };

  void ThreadMain();
  void BindEngineNatives();          // timers + debug, captured on this guest
  void ScheduleUpdate(papyrus::ObjectRef target, f64 due, f64 interval);
  void CancelUpdate(papyrus::ObjectRef target);
  void AdvanceUpdates(f64 dt);       // guest thread only
  // Game-time timers: due/interval are in game days, fired when the world clock
  // (via game_time_provider_) passes them. Mirror the real-time set above.
  void ScheduleGameUpdate(papyrus::ObjectRef target, f64 due, f64 interval);
  void CancelGameUpdate(papyrus::ObjectRef target);
  void AdvanceGameUpdates(f64 now);  // guest thread only
  void AddLosWatch(LosWatch watch);
  void RemoveLosWatch(papyrus::ObjectRef registrant, papyrus::ObjectRef viewer,
                      papyrus::ObjectRef target);
  void AdvanceLosWatches();  // guest thread only
  f64 GameNow() const { return game_time_provider_ ? game_time_provider_() : 0.0; }

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
  // TODO: serialize these into a savegame once one exists. Each entry is plain data
  // (target handle + due/interval, or registrant/viewer/target), so they survive a
  // save directly, unlike the fiber-parked Waits in fiber_sched_.
  std::vector<ScheduledUpdate> updates_;
  std::vector<ScheduledUpdate> game_updates_;
  std::vector<LosWatch> los_watches_;
  // Runs activations on fibers and parks the ones that hit a latent Wait. Pulls
  // each suspending activation's delay from the VM's latent request.
  papyrus::FiberScheduler fiber_sched_{[this] { return vm_.TakeLatentRequest(); }};

  // Set once on the guest thread (see set_game_time_provider); read on the guest
  // thread when scheduling and firing game-time timers.
  std::function<f64()> game_time_provider_;

  // Set once on the guest thread (see set_los_provider); read on the guest thread
  // when evaluating line-of-sight watches.
  std::function<bool(u64, u64)> los_provider_;

  // Set once on the guest thread (see set_alias_resolver); consulted by RaiseEvent
  // to also dispatch an event to the aliases the target ref fills.
  std::function<std::vector<papyrus::ObjectRef>(papyrus::ObjectRef)> alias_resolver_;

  // Set once on the guest thread (see set_on_notification); read by the
  // Debug.Notification native, also on the guest thread.
  std::function<void(const std::string&)> on_notification_;

  // Set once on the guest thread (see set_on_debug_command); read by the Debug.*
  // engine-command natives, also on the guest thread.
  std::function<void(const std::string&, const std::string&)> on_debug_command_;

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

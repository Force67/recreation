#include "script/papyrus_guest.h"

#include <algorithm>

#include "core/log.h"

namespace rec::script {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;

PapyrusGuest::PapyrusGuest(bethesda::Game game) : game_(game), vm_(&natives_) {
  BindEngineNatives();
}

PapyrusGuest::~PapyrusGuest() { Stop(); }

void PapyrusGuest::Start() {
  if (running_) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = false;
  }
  running_ = true;
  thread_ = std::thread([this] { ThreadMain(); });
}

void PapyrusGuest::Stop() {
  if (!running_) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void PapyrusGuest::ThreadMain() {
  for (;;) {
    MoveOnlyFunction<void(VirtualMachine&)> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stop_) return;
        continue;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    job(vm_);
  }
}

void PapyrusGuest::Submit(MoveOnlyFunction<void(VirtualMachine&)> fn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(fn));
  }
  wake_.notify_one();
}

std::future<std::string> PapyrusGuest::LoadScript(std::vector<u8> pex) {
  return SubmitFor([pex = std::move(pex)](VirtualMachine& vm) {
    return vm.LoadScript(ByteSpan(pex.data(), pex.size()));
  });
}

std::future<ObjectRef> PapyrusGuest::CreateInstance(std::string type) {
  return SubmitFor([type = std::move(type)](VirtualMachine& vm) { return vm.CreateInstance(type); });
}

void PapyrusGuest::RaiseEvent(ObjectRef target, std::string event, std::vector<Value> args) {
  // Events are optional handlers; TryCall dispatches only if the target defines
  // one and never warns otherwise, so broadcasting to every form is cheap.
  Submit([this, target, event = std::move(event),
          args = std::move(args)](VirtualMachine& vm) mutable {
    RunScript([this, &vm, target, event = std::move(event), args = std::move(args)]() mutable {
      vm.TryCall(target, event, args);
      // Also reach the scripts on any quest alias the target fills, so an alias's
      // OnActivate/OnTriggerEnter handler runs (native-raised events already route
      // this way through the bindings). Copy args: TryCall consumes its vector.
      if (alias_resolver_)
        for (ObjectRef alias : alias_resolver_(target)) vm.TryCall(alias, event, args);
    });
  });
}

void PapyrusGuest::Tick(f32 dt) {
  Submit([this, dt](VirtualMachine&) { AdvanceUpdates(dt); });
}

void PapyrusGuest::ScheduleUpdate(ObjectRef target, f64 due, f64 interval) {
  CancelUpdate(target);
  updates_.push_back({target, due, interval});
}

void PapyrusGuest::CancelUpdate(ObjectRef target) {
  std::erase_if(updates_, [&](const ScheduledUpdate& u) { return u.target == target; });
}

void PapyrusGuest::ScheduleGameUpdate(ObjectRef target, f64 due, f64 interval) {
  CancelGameUpdate(target);
  game_updates_.push_back({target, due, interval});
}

void PapyrusGuest::CancelGameUpdate(ObjectRef target) {
  std::erase_if(game_updates_, [&](const ScheduledUpdate& u) { return u.target == target; });
}

void PapyrusGuest::AdvanceGameUpdates(f64 now) {
  // Same snapshot-then-fire discipline as AdvanceUpdates: a handler may reschedule
  // itself and must not fire again this tick.
  std::vector<ObjectRef> due;
  for (ScheduledUpdate& u : game_updates_)
    if (u.due <= now) due.push_back(u.target);
  for (ObjectRef target : due) {
    auto it = std::find_if(game_updates_.begin(), game_updates_.end(),
                           [&](const ScheduledUpdate& u) { return u.target == target; });
    if (it == game_updates_.end()) continue;
    f64 interval = it->interval;
    if (interval > 0)
      it->due += interval;
    else
      game_updates_.erase(it);
    RunScript([this, target] { vm_.Call(target, "OnUpdateGameTime", {}); });
  }
}

void PapyrusGuest::AddLosWatch(LosWatch watch) {
  RemoveLosWatch(watch.registrant, watch.viewer, watch.target);
  los_watches_.push_back(watch);
}

void PapyrusGuest::RemoveLosWatch(ObjectRef registrant, ObjectRef viewer, ObjectRef target) {
  std::erase_if(los_watches_, [&](const LosWatch& w) {
    return w.registrant == registrant && w.viewer == viewer && w.target == target;
  });
}

void PapyrusGuest::AdvanceLosWatches() {
  if (!los_provider_ || los_watches_.empty()) return;
  // Evaluate against a snapshot index: a handler may register or drop watches, so
  // collect the events first, then fire, then prune the one-shots that fired.
  struct Fired {
    ObjectRef registrant, viewer, target;
    bool gained;
  };
  std::vector<Fired> fired;
  std::vector<size_t> drop;
  for (size_t i = 0; i < los_watches_.size(); ++i) {
    LosWatch& w = los_watches_[i];
    bool now = los_provider_(w.viewer.handle, w.target.handle);
    if (now == w.has_los) continue;
    w.has_los = now;
    const bool wants = (now && w.mode != LosMode::kSingleLost) ||
                       (!now && w.mode != LosMode::kSingleGain);
    if (!wants) continue;
    fired.push_back({w.registrant, w.viewer, w.target, now});
    if (w.mode != LosMode::kBoth) drop.push_back(i);
  }
  for (auto it = drop.rbegin(); it != drop.rend(); ++it) los_watches_.erase(los_watches_.begin() + *it);
  for (const Fired& f : fired)
    RunScript([this, f] {
      vm_.TryCall(f.registrant, f.gained ? "OnGainLOS" : "OnLostLOS",
                  {Value::Object(f.viewer), Value::Object(f.target)});
    });
}

void PapyrusGuest::RunScript(std::function<void()> body) {
  fiber_sched_.Run(std::move(body), clock_, GameNow());
}

void PapyrusGuest::AdvanceUpdates(f64 dt) {
  clock_ += dt;
  // Resume any activations whose Wait has elapsed before firing this tick's work.
  fiber_sched_.Advance(clock_, GameNow());
  if (game_time_provider_) AdvanceGameUpdates(game_time_provider_());
  AdvanceLosWatches();
  // Snapshot the due set first: an OnUpdate handler may reschedule itself, and
  // we must not fire that new registration in the same tick.
  std::vector<ObjectRef> due;
  for (ScheduledUpdate& u : updates_)
    if (u.due <= clock_) due.push_back(u.target);
  for (ObjectRef target : due) {
    auto it = std::find_if(updates_.begin(), updates_.end(),
                           [&](const ScheduledUpdate& u) { return u.target == target; });
    if (it == updates_.end()) continue;
    f64 interval = it->interval;
    if (interval > 0)
      it->due += interval;
    else
      updates_.erase(it);
    RunScript([this, target] { vm_.Call(target, "OnUpdate", {}); });
  }
}

void PapyrusGuest::BindEngineNatives() {
  // Timer natives. Skyrim declares them separately on Form, Alias and
  // ActiveMagicEffect (they are distinct base classes), so register under each:
  // a ReferenceAlias-derived script (the Civil War per-soldier behavior layer)
  // reaches them through Alias, not Form. They schedule against this guest's clock.
  auto reg_single = [this](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
    f64 secs = args.empty() ? 0 : args[0].ToFloat();
    ScheduleUpdate(self, clock_ + secs, 0);
    return Value();
  };
  auto reg_update = [this](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
    f64 secs = args.empty() ? 0 : args[0].ToFloat();
    ScheduleUpdate(self, clock_ + secs, secs > 0 ? secs : 0);
    return Value();
  };
  auto unreg = [this](VirtualMachine&, ObjectRef self, std::vector<Value>&) {
    CancelUpdate(self);
    return Value();
  };
  for (const char* type : {"Form", "Alias", "ActiveMagicEffect"}) {
    natives_.Register(type, "RegisterForSingleUpdate", reg_single);
    natives_.Register(type, "RegisterForUpdate", reg_update);
    natives_.Register(type, "UnregisterForUpdate", unreg);
  }

  // Game-time timers. The interval is in game hours; the world clock (game_days)
  // is the reference, so a script can wait a number of in-game hours and receive
  // OnUpdateGameTime. Without a provider these never fire.
  auto game_now = [this] { return game_time_provider_ ? game_time_provider_() : 0.0; };
  auto reg_gt_single = [this, game_now](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
    f64 hours = args.empty() ? 0 : args[0].ToFloat();
    ScheduleGameUpdate(self, game_now() + hours / 24.0, 0);
    return Value();
  };
  auto reg_gt_update = [this, game_now](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
    f64 interval = (args.empty() ? 0 : args[0].ToFloat()) / 24.0;
    ScheduleGameUpdate(self, game_now() + interval, interval > 0 ? interval : 0);
    return Value();
  };
  auto unreg_gt = [this](VirtualMachine&, ObjectRef self, std::vector<Value>&) {
    CancelGameUpdate(self);
    return Value();
  };
  for (const char* type : {"Form", "Alias", "ActiveMagicEffect"}) {
    natives_.Register(type, "RegisterForSingleUpdateGameTime", reg_gt_single);
    natives_.Register(type, "RegisterForUpdateGameTime", reg_gt_update);
    natives_.Register(type, "UnregisterForUpdateGameTime", unreg_gt);
  }

  // Line-of-sight watches. RegisterForLOS(akViewer, akTarget) fires OnGainLOS and
  // OnLostLOS on the registering script as the viewer's sight of the target
  // changes; the single variants fire once for one direction. Seed each watch with
  // the current state so only a transition fires.
  auto los_now = [this](ObjectRef v, ObjectRef t) {
    return los_provider_ ? los_provider_(v.handle, t.handle) : false;
  };
  auto make_los = [this, los_now](LosMode mode) {
    return [this, los_now, mode](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
      if (args.size() < 2) return Value();
      ObjectRef viewer = args[0].as_object();
      ObjectRef target = args[1].as_object();
      AddLosWatch({self, viewer, target, mode, los_now(viewer, target)});
      return Value();
    };
  };
  auto unreg_los = [this](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
    if (args.size() >= 2) RemoveLosWatch(self, args[0].as_object(), args[1].as_object());
    return Value();
  };
  for (const char* type : {"Form", "Alias", "ActiveMagicEffect"}) {
    natives_.Register(type, "RegisterForLOS", make_los(LosMode::kBoth));
    natives_.Register(type, "RegisterForSingleLOSGain", make_los(LosMode::kSingleGain));
    natives_.Register(type, "RegisterForSingleLOSLost", make_los(LosMode::kSingleLost));
    natives_.Register(type, "UnregisterForLOS", unreg_los);
  }

  // Debug output: the most common engine-independent Papyrus natives.
  auto trace = [](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
    REC_INFO("[papyrus] {}", args.empty() ? "" : args[0].ToString());
    return Value();
  };
  natives_.Register("Debug", "Trace", trace);
  // TraceUser writes to a named user log (arg 1); keep the channel in the line.
  natives_.Register("Debug", "TraceUser",
                    [](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
                      const std::string log = args.size() > 1 ? args[1].ToString() : "user";
                      REC_INFO("[papyrus:{}] {}", log, args.empty() ? "" : args[0].ToString());
                      return Value();
                    });
  // A message box and a notification both surface on the HUD when the runtime
  // wires a handler, and the trace log either way for diagnostics.
  auto surface = [this](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
    const std::string message = args.empty() ? "" : args[0].ToString();
    REC_INFO("[papyrus] notification: {}", message);
    if (on_notification_) on_notification_(message);
    return Value();
  };
  natives_.Register("Debug", "MessageBox", surface);
  natives_.Register("Debug", "Notification", surface);

  // Engine-reaching Debug commands: quit, screenshot, and the global dev toggles.
  // Each forwards a verb to the runtime, which applies the real action on the main
  // loop. Registered here because Debug is shared across every game's guest.
  auto cmd = [this](const char* verb) {
    std::string v = verb;
    natives_.Register("Debug", v.c_str(),
                      [this, v](VirtualMachine&, ObjectRef, std::vector<Value>&) {
                        if (on_debug_command_) on_debug_command_(v, "");
                        return Value();
                      });
  };
  cmd("QuitGame");
  cmd("TakeScreenshot");
  cmd("ToggleCollisions");
  cmd("ToggleAI");
  cmd("ToggleMenus");
  auto flag_cmd = [this](const char* verb) {
    std::string v = verb;
    natives_.Register("Debug", v.c_str(),
                      [this, v](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
                        const bool on = args.empty() ? false : args[0].ToBool();
                        if (on_debug_command_) on_debug_command_(v, on ? "1" : "0");
                        return Value();
                      });
  };
  flag_cmd("SetGodMode");
  flag_cmd("SetFootIK");

  // Multiplayer platform HUD/Net surface. The C# platform (chat, notifications,
  // prompts, scoreboard, blips, server browser) reaches the engine through these
  // game-agnostic globals; each forwards to the runtime's PlatformHud sink, which
  // the main loop drains onto the on-screen HUD. Registering them here means a
  // server's UI works the same in every game's guest.
  auto reg_platform = [this](const char* type, const char* func) {
    std::string t = type;
    std::string f = func;
    natives_.Register(type, func,
                      [this, t, f](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
                        if (on_platform_hud_) on_platform_hud_(t, f, args);
                        return Value();
                      });
  };
  for (const char* f :
       {"Notify", "ChatLine", "Prompt", "ClearPrompt", "Blip", "ClearBlip", "Waypoint",
        "ClearWaypoint", "Nametag", "Scoreboard", "ScoreboardRow", "HideScoreboard", "Menu"}) {
    reg_platform("Hud", f);
  }
  for (const char* f : {"Connect", "SpawnObject", "MoveObject", "DeleteObject"}) {
    reg_platform("Net", f);
  }
  // Net.LocalPos{X,Y,Z}(): the local player's world position (engine space) so a
  // mod can place things relative to the player. These return a value, unlike the
  // fire-and-forget calls above.
  for (int axis = 0; axis < 3; ++axis) {
    const char* name = axis == 0 ? "LocalPosX" : (axis == 1 ? "LocalPosY" : "LocalPosZ");
    natives_.Register("Net", name, [this, axis](VirtualMachine&, ObjectRef, std::vector<Value>&) {
      return Value::Float(local_pos_provider_ ? local_pos_provider_()[axis] : 0.0f);
    });
  }
}

}  // namespace rec::script

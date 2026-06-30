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
  Submit([target, event = std::move(event), args = std::move(args)](VirtualMachine& vm) mutable {
    vm.TryCall(target, event, std::move(args));
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

void PapyrusGuest::AdvanceUpdates(f64 dt) {
  clock_ += dt;
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
    vm_.Call(target, "OnUpdate", {});
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

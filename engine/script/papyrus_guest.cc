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
    std::move_only_function<void(VirtualMachine&)> job;
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

void PapyrusGuest::Submit(std::move_only_function<void(VirtualMachine&)> fn) {
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
  Submit([target, event = std::move(event), args = std::move(args)](VirtualMachine& vm) mutable {
    vm.Call(target, event, std::move(args));
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
  // Timer natives live on Form in Skyrim; an instance reaches them through its
  // inheritance chain. They schedule against this guest's clock.
  natives_.Register("Form", "RegisterForSingleUpdate",
                    [this](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
                      f64 secs = args.empty() ? 0 : args[0].ToFloat();
                      ScheduleUpdate(self, clock_ + secs, 0);
                      return Value();
                    });
  natives_.Register("Form", "RegisterForUpdate",
                    [this](VirtualMachine&, ObjectRef self, std::vector<Value>& args) {
                      f64 secs = args.empty() ? 0 : args[0].ToFloat();
                      ScheduleUpdate(self, clock_ + secs, secs > 0 ? secs : 0);
                      return Value();
                    });
  natives_.Register("Form", "UnregisterForUpdate",
                    [this](VirtualMachine&, ObjectRef self, std::vector<Value>&) {
                      CancelUpdate(self);
                      return Value();
                    });

  // Debug output: the most common engine-independent Papyrus natives.
  auto trace = [](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
    REC_INFO("[papyrus] {}", args.empty() ? "" : args[0].ToString());
    return Value();
  };
  natives_.Register("Debug", "Trace", trace);
  natives_.Register("Debug", "TraceUser", trace);
  natives_.Register("Debug", "Notification", trace);
  natives_.Register("Debug", "MessageBox", trace);
}

}  // namespace rec::script

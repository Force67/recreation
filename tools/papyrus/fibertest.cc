// fibertest: the phase-1 spike for latent Papyrus suspension. It proves the fiber
// primitive can freeze a C++ call chain mid-execution and resume it, including
// when the suspend point is several calls deep and routed through the VM's
// SuspendCurrent hook (the seam Utility.Wait will use). No game assets needed.

#include <cstdio>

#include <vector>

#include "script/games/skyrim/skyrim_natives.h"
#include "script/papyrus/fiber.h"
#include "script/papyrus/fiber_scheduler.h"
#include "script/papyrus/native.h"
#include "script/papyrus/value.h"
#include "script/papyrus/vm.h"

using rec::script::papyrus::Fiber;
using rec::script::papyrus::FiberScheduler;
using rec::script::papyrus::NativeFunction;
using rec::script::papyrus::NativeRegistry;
using rec::script::papyrus::ObjectRef;
using rec::script::papyrus::Value;
using rec::script::papyrus::VirtualMachine;
using rec::script::skyrim::SkyrimBindings;

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // 1. The bare primitive: local state survives a yield and a resume.
  {
    int counter = 0;
    Fiber f([&] {
      counter = 1;
      Fiber::YieldCurrent();
      counter = 2;
    });
    f.Resume();
    check("fiber runs up to its first yield", counter == 1 && !f.done());
    f.Resume();
    check("fiber resumes past the yield to completion", counter == 2 && f.done());
  }

  // 2. A nested C++ chain suspends through the VM hook, and a local several frames
  //    down survives the round trip. This is the Wait scenario: Fragment -> helper
  //    -> Utility.Wait, frozen mid-stack and continued later.
  {
    NativeRegistry reg;
    VirtualMachine vm(&reg);
    int stage = 0;
    auto deep = [&](VirtualMachine& v) {
      int secret = 1234;  // a local that must survive the suspend
      stage = 1;
      v.SuspendCurrent();
      stage = secret;
    };
    auto outer = [&](VirtualMachine& v) { deep(v); };
    Fiber fib([&] { outer(vm); });
    fib.Resume();
    check("nested call suspends mid-stack", stage == 1 && !fib.done());
    // The scheduler is free here; the activation is parked with its stack intact.
    fib.Resume();
    check("nested call resumes and the deep local survived", stage == 1234 && fib.done());
  }

  // 3. Independent activations interleave: B can run to completion while A is
  //    still parked, then A resumes. This is what lets other scripts run during a
  //    Wait.
  {
    NativeRegistry reg;
    VirtualMachine vm(&reg);
    int a = 0, b = 0;
    Fiber fa([&] {
      a = 1;
      vm.SuspendCurrent();
      a = 2;
    });
    Fiber fb([&] {
      b = 1;
      vm.SuspendCurrent();
      b = 2;
    });
    fa.Resume();
    fb.Resume();
    check("both activations suspended", a == 1 && b == 1 && !fa.done() && !fb.done());
    fb.Resume();
    check("B completes while A stays parked", b == 2 && fb.done() && a == 1 && !fa.done());
    fa.Resume();
    check("A completes independently afterwards", a == 2 && fa.done());
  }

  // 4. SuspendCurrent is a safe no-op at the top level (no fiber running).
  {
    NativeRegistry reg;
    VirtualMachine vm(&reg);
    check("SuspendCurrent off a fiber is a no-op", !vm.SuspendCurrent());
  }

  // 5. The scheduler parks a real-time Wait and resumes it after the deadline,
  //    driven end to end through the VM's SuspendCurrentFor / TakeLatentRequest.
  {
    NativeRegistry reg;
    VirtualMachine vm(&reg);
    FiberScheduler sched([&] { return vm.TakeLatentRequest(); });
    int step = 0;
    bool parked = sched.Run(
        [&] {
          step = 1;
          vm.SuspendCurrentFor(0.5, -1.0);  // wait 0.5 real seconds
          step = 2;
        },
        /*real_now=*/0.0, /*game_now=*/0.0);
    check("activation parks at the Wait", parked && step == 1 && sched.parked() == 1);
    sched.Advance(0.4, 0.0);
    check("not resumed before the deadline", step == 1 && sched.parked() == 1);
    sched.Advance(0.6, 0.0);
    check("resumed after the deadline", step == 2 && sched.parked() == 0);
  }

  // 6. A game-time Wait resumes off game days, ignoring real seconds.
  {
    NativeRegistry reg;
    VirtualMachine vm(&reg);
    FiberScheduler sched([&] { return vm.TakeLatentRequest(); });
    bool done = false;
    sched.Run([&] { vm.SuspendCurrentFor(-1.0, 0.25); done = true; }, /*real=*/0.0, /*game=*/10.0);
    check("game-time wait parks", !done && sched.parked() == 1);
    sched.Advance(1e6, 10.2);  // huge real time, but game day < deadline
    check("real time does not wake a game-time wait", !done && sched.parked() == 1);
    sched.Advance(1e6, 10.3);  // game day 10.3 >= 10.0 + 0.25
    check("game-time wait resumes at its game deadline", done && sched.parked() == 0);
  }

  // 7. Two waits in a row: the activation re-parks and resumes again.
  {
    NativeRegistry reg;
    VirtualMachine vm(&reg);
    FiberScheduler sched([&] { return vm.TakeLatentRequest(); });
    int step = 0;
    sched.Run(
        [&] {
          step = 1;
          vm.SuspendCurrentFor(1.0, -1.0);
          step = 2;
          vm.SuspendCurrentFor(1.0, -1.0);
          step = 3;
        },
        0.0, 0.0);
    check("first wait parks", step == 1 && sched.parked() == 1);
    sched.Advance(1.5, 0.0);
    check("resumes to the second wait and re-parks", step == 2 && sched.parked() == 1);
    sched.Advance(3.0, 0.0);
    check("second wait resumes to completion", step == 3 && sched.parked() == 0);
  }

  // 8. A non-waiting activation runs to completion now and is never parked.
  {
    NativeRegistry reg;
    VirtualMachine vm(&reg);
    FiberScheduler sched([&] { return vm.TakeLatentRequest(); });
    bool ran = false;
    bool parked = sched.Run([&] { ran = true; }, 0.0, 0.0);
    check("non-waiting activation completes immediately", ran && !parked && sched.parked() == 0);
  }

  // 9. The real Utility.Wait native (registered through RegisterSkyrimNatives)
  //    suspends the activation and resumes it after its real-time delay.
  {
    SkyrimBindings binds;
    NativeRegistry reg;
    rec::script::skyrim::RegisterSkyrimNatives(reg, &binds);
    VirtualMachine vm(&reg);
    FiberScheduler sched([&] { return vm.TakeLatentRequest(); });
    const NativeFunction* wait = reg.Find("Utility", "Wait");
    int step = 0;
    sched.Run(
        [&] {
          step = 1;
          std::vector<Value> args = {Value::Float(2.0f)};  // Utility.Wait(2.0)
          (*wait)(vm, ObjectRef{}, args);
          step = 2;
        },
        /*real_now=*/0.0, /*game_now=*/0.0);
    check("real Utility.Wait parks the activation", step == 1 && sched.parked() == 1);
    sched.Advance(1.0, 0.0);
    check("Utility.Wait stays parked mid-delay", step == 1);
    sched.Advance(2.5, 0.0);
    check("Utility.Wait resumes after its delay", step == 2 && sched.parked() == 0);
  }

  // 10. Utility.WaitGameTime resumes off game days (its argument is game hours).
  {
    SkyrimBindings binds;
    NativeRegistry reg;
    rec::script::skyrim::RegisterSkyrimNatives(reg, &binds);
    VirtualMachine vm(&reg);
    FiberScheduler sched([&] { return vm.TakeLatentRequest(); });
    const NativeFunction* wait_gt = reg.Find("Utility", "WaitGameTime");
    bool done = false;
    sched.Run(
        [&] {
          std::vector<Value> args = {Value::Float(12.0f)};  // 12 game hours = 0.5 days
          (*wait_gt)(vm, ObjectRef{}, args);
          done = true;
        },
        /*real_now=*/0.0, /*game_now=*/100.0);
    check("WaitGameTime parks", !done && sched.parked() == 1);
    sched.Advance(1e9, 100.4);  // 0.4 game days < 0.5
    check("WaitGameTime ignores real time and waits the game delay", !done);
    sched.Advance(1e9, 100.5);  // 0.5 game days reached
    check("WaitGameTime resumes at its game deadline", done && sched.parked() == 0);
  }

  std::printf("%s (%d failures)\n", failures ? "FIBERTEST FAILED" : "FIBERTEST PASSED", failures);
  return failures ? 1 : 0;
}

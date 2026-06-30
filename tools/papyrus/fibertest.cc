// fibertest: the phase-1 spike for latent Papyrus suspension. It proves the fiber
// primitive can freeze a C++ call chain mid-execution and resume it, including
// when the suspend point is several calls deep and routed through the VM's
// SuspendCurrent hook (the seam Utility.Wait will use). No game assets needed.

#include <cstdio>

#include "script/papyrus/fiber.h"
#include "script/papyrus/native.h"
#include "script/papyrus/vm.h"

using rec::script::papyrus::Fiber;
using rec::script::papyrus::NativeRegistry;
using rec::script::papyrus::VirtualMachine;

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

  std::printf("%s (%d failures)\n", failures ? "FIBERTEST FAILED" : "FIBERTEST PASSED", failures);
  return failures ? 1 : 0;
}

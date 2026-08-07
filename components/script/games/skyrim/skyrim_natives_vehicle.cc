#include "components/script/games/skyrim/skyrim_natives_ext.h"

namespace rx::script::skyrim {

using ext::ArgB;
using ext::ArgF;
using ext::ArgI;
using ext::Args;
using ext::Resolve;
using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;

// The cart racing kit: the three primitives a racing ruleset needs that managed
// code cannot fake. `Input.Held` reads the held-key snapshot the runtime pushes
// each frame; `Vehicle.Drive` routes through the runtime's optional sink to the
// ridden cart; `Vehicle.Speed` reads the ridden-cart speed snapshot. All three
// are neutral no-ops when the ride or the runtime wiring is absent, so every
// other game path is unaffected.
void RegisterVehicleExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Input", "Held", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Bool(Resolve(bindings).InputHeld(ArgI(a, 0)));
  });

  reg.Register("Vehicle", "Drive", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    Resolve(bindings).DriveCart(ArgF(a, 0), ArgF(a, 1));
    return Value();
  });

  reg.Register("Vehicle", "Speed", [bindings](VirtualMachine&, ObjectRef, Args&) {
    return Value::Float(Resolve(bindings).CartSpeed());
  });

  reg.Register("Vehicle", "Riding", [bindings](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(Resolve(bindings).IsRiding());
  });

  reg.Register("Vehicle", "MoveTo", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    Resolve(bindings).MoveCart(ArgF(a, 0), ArgF(a, 1), ArgF(a, 2));
    return Value();
  });
}

}  // namespace rx::script::skyrim

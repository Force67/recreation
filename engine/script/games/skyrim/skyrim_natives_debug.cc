#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgB;
using ext::ArgF;
using ext::ArgI;
using ext::ArgO;
using ext::ArgS;
using ext::Resolve;
namespace st = state;

void RegisterDebugExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Developer console commands with no script-observable result; wired as no-ops
  // so scripts that call them keep running.
  auto cmd = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Debug", "CenterOnCell", cmd);
  reg.Register("Debug", "CenterOnCellAndWait", cmd);
  reg.Register("Debug", "CloseUserLog", cmd);
  reg.Register("Debug", "DBSendPlayerPosition", cmd);
  reg.Register("Debug", "DebugChannelNotify", cmd);
  reg.Register("Debug", "DumpAliasData", cmd);
  reg.Register("Debug", "OpenUserLog", cmd);
  reg.Register("Debug", "PlayerMoveToAndWait", cmd);
  reg.Register("Debug", "SendAnimationEvent", cmd);
  reg.Register("Debug", "ShowRefPosition", cmd);
  reg.Register("Debug", "StartScriptProfiling", cmd);
  reg.Register("Debug", "StartStackProfiling", cmd);
  reg.Register("Debug", "StopScriptProfiling", cmd);
  reg.Register("Debug", "StopStackProfiling", cmd);

  reg.Register("Debug", "GetConfigName",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Str(""); });
  // QuitGame, TakeScreenshot, the global dev toggles (ToggleAI/Collisions/Menus,
  // SetGodMode, SetFootIK) reach the engine and live in papyrus_guest.cc, which
  // owns the runtime command hook. Registering them here would shadow that.
}

}  // namespace rec::script::skyrim

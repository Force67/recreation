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

void RegisterGameExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Process-wide Game flags and counters have no owning form, so they key off a
  // fixed sentinel owner. A set then get round-trips through the shared store.
  const ObjectRef kGame{1};

  auto flag = [kGame](const char* key) {
    return [kGame, key](VirtualMachine&, ObjectRef, Args& a) {
      st::SetFlag(kGame, key, ArgB(a, 0, false));
      return Value();
    };
  };
  reg.Register("Game", "SetInChargen", flag("inChargen"));
  reg.Register("Game", "SetBeastForm", flag("beastForm"));
  reg.Register("Game", "SetPlayerAIDriven", flag("aiDriven"));
  reg.Register("Game", "SetHudCartMode", flag("hudCartMode"));
  reg.Register("Game", "SetPlayerReportCrime", flag("playerReportCrime"));
  reg.Register("Game", "SetAllowFlyingMountLandingRequests", flag("allowFlyingMountLanding"));

  reg.Register("Game", "AddPerkPoints", [kGame](VirtualMachine&, ObjectRef, Args& a) {
    st::SetInt(kGame, "perkPoints", st::GetInt(kGame, "perkPoints") + ArgI(a, 0));
    return Value();
  });

  // Stat and skill bookkeeping: no stat subsystem yet, so the mutators are
  // no-ops and the queries report a neutral zero.
  auto stat_noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Game", "IncrementStat", stat_noop);
  reg.Register("Game", "IncrementSkill", stat_noop);
  reg.Register("Game", "IncrementSkillBy", stat_noop);
  reg.Register("Game", "AdvanceSkill", stat_noop);
  reg.Register("Game", "QueryStat",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Game", "GetGameSettingString",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Str(""); });
  reg.Register("Game", "CalculateFavorCost",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Game", "IsWordUnlocked", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Bool(st::GetFlag(ArgO(a, 0), "wordUnlocked", false));
  });
  reg.Register("Game", "IsPlayerSungazing",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });

  // Lookups with no data source behind them yet resolve to None.
  auto none_obj = [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Object(ObjectRef{});
  };
  reg.Register("Game", "GetFormFromFile", none_obj);
  reg.Register("Game", "GetPlayerGrabbedRef", none_obj);
  reg.Register("Game", "GetPlayersLastRiddenHorse", none_obj);
  reg.Register("Game", "FindClosestActor", none_obj);
  reg.Register("Game", "FindClosestReferenceOfType", none_obj);
  reg.Register("Game", "FindClosestReferenceOfAnyTypeInList", none_obj);
  reg.Register("Game", "FindRandomActor", none_obj);
  reg.Register("Game", "FindRandomReferenceOfType", none_obj);
  reg.Register("Game", "FindRandomReferenceOfAnyTypeInList", none_obj);

  // Pure engine commands (camera, menus, fast travel, saves, havok, words) have
  // no script-observable result and stay no-ops until their subsystems exist.
  auto cmd = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Game", "FadeOutGame", cmd);
  reg.Register("Game", "FastTravel", cmd);
  reg.Register("Game", "ForceFirstPerson", cmd);
  reg.Register("Game", "ForceThirdPerson", cmd);
  reg.Register("Game", "ShowFirstPersonGeometry", cmd);
  reg.Register("Game", "ShakeCamera", cmd);
  reg.Register("Game", "ShakeController", cmd);
  reg.Register("Game", "SetCameraTarget", cmd);
  reg.Register("Game", "ShowRaceMenu", cmd);
  reg.Register("Game", "ShowLimitedRaceMenu", cmd);
  reg.Register("Game", "ShowTrainingMenu", cmd);
  reg.Register("Game", "ShowTitleSequenceMenu", cmd);
  reg.Register("Game", "HideTitleSequenceMenu", cmd);
  reg.Register("Game", "StartTitleSequence", cmd);
  reg.Register("Game", "PlayBink", cmd);
  reg.Register("Game", "TriggerScreenBlood", cmd);
  reg.Register("Game", "QuitToMainMenu", cmd);
  reg.Register("Game", "RequestSave", cmd);
  reg.Register("Game", "RequestAutoSave", cmd);
  reg.Register("Game", "RequestModel", cmd);
  reg.Register("Game", "PrecacheCharGen", cmd);
  reg.Register("Game", "PrecacheCharGenClear", cmd);
  reg.Register("Game", "ClearPrison", cmd);
  reg.Register("Game", "ClearTempEffects", cmd);
  reg.Register("Game", "ServeTime", cmd);
  reg.Register("Game", "SendWereWolfTransformation", cmd);
  reg.Register("Game", "AddAchievement", cmd);
  reg.Register("Game", "AddHavokBallAndSocketConstraint", cmd);
  reg.Register("Game", "RemoveHavokConstraints", cmd);
  reg.Register("Game", "SetSittingRotation", cmd);
  reg.Register("Game", "SetSunGazeImageSpaceModifier", cmd);
  // A word of power is first taught (it appears in the shout list) and later
  // unlocked (usable). IsWordUnlocked reads the unlocked flag, so track both.
  reg.Register("Game", "TeachWord", [](VirtualMachine&, ObjectRef, Args& a) {
    st::SetFlag(ArgO(a, 0), "wordTaught", true);
    return Value();
  });
  reg.Register("Game", "UnlockWord", [](VirtualMachine&, ObjectRef, Args& a) {
    st::SetFlag(ArgO(a, 0), "wordUnlocked", true);
    return Value();
  });
}

}  // namespace rec::script::skyrim

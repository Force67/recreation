// nativesexttest: the computed Skyrim natives added on top of the binding
// surface (sun position, game-time formatting, actor-value max, container
// queries). Drives them through the native registry against a mock binding, so
// it needs no game assets and asserts exact values.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "script/games/skyrim/skyrim_natives.h"
#include "script/papyrus/native.h"
#include "script/papyrus/value.h"
#include "script/papyrus/vm.h"
#include "script/papyrus_guest.h"

namespace {

using namespace rec;
using rec::script::papyrus::NativeFunction;
using rec::script::papyrus::NativeRegistry;
using rec::script::papyrus::ObjectRef;
using rec::script::papyrus::Value;
using rec::script::papyrus::VirtualMachine;
using rec::script::skyrim::SkyrimBindings;

// A binding with just the slices the computed natives read, seeded with fixed
// values so the outputs are exact.
class MockBindings : public SkyrimBindings {
 public:
  f32 game_time = 0.0f;
  f32 base_health = 0.0f;
  std::vector<std::pair<ObjectRef, i32>> inventory;
  std::vector<ObjectRef> list_forms;  // base FLST entries
  i32 crime_gold = 0;
  std::vector<std::pair<ObjectRef, bool>> effects;  // (effect, detrimental) for the item under test
  ObjectRef last_played;
  i32 voice_id = 7;
  ObjectRef follow_actor;
  bool following = false;

  f32 GetCurrentGameTime() override { return game_time; }
  f32 GetGameSettingFloat(const std::string&) override { return 42.7f; }
  f32 GetBaseActorValue(ObjectRef, const std::string&) override { return base_health; }
  i32 GetCrimeGold(ObjectRef) override { return crime_gold; }
  i32 GetMagicEffectCount(ObjectRef) override { return static_cast<i32>(effects.size()); }
  ObjectRef GetNthMagicEffectId(i32 index) override {
    return index >= 0 && index < static_cast<i32>(effects.size()) ? effects[index].first
                                                                   : ObjectRef{};
  }
  bool GetMagicEffectDetrimental(ObjectRef effect) override {
    for (const auto& [e, det] : effects)
      if (e.handle == effect.handle) return det;
    return false;
  }
  i32 PlaySound(ObjectRef sound, ObjectRef) override {
    last_played = sound;
    return voice_id;
  }
  void SetActorFollowing(ObjectRef actor, bool follow) override {
    follow_actor = actor;
    following = follow;
  }
  i32 GetFormListSize(ObjectRef) override { return static_cast<i32>(list_forms.size()); }
  ObjectRef GetNthListForm(i32 index) override {
    return index >= 0 && index < static_cast<i32>(list_forms.size()) ? list_forms[index]
                                                                     : ObjectRef{};
  }
  i32 GetNumItems(ObjectRef) override { return static_cast<i32>(inventory.size()); }
  ObjectRef GetNthForm(ObjectRef, i32 index) override {
    return index >= 0 && index < static_cast<i32>(inventory.size()) ? inventory[index].first
                                                                     : ObjectRef{};
  }
  i32 GetItemCount(ObjectRef, ObjectRef item) override {
    for (const auto& [form, count] : inventory)
      if (form.handle == item.handle) return count;
    return 0;
  }
  void RemoveItem(ObjectRef, ObjectRef item, i32) override {
    for (auto it = inventory.begin(); it != inventory.end(); ++it)
      if (it->first.handle == item.handle) {
        inventory.erase(it);
        return;
      }
  }
};

}  // namespace

int main() {
  MockBindings bindings;
  NativeRegistry reg;
  rec::script::skyrim::RegisterSkyrimNatives(reg, &bindings);
  VirtualMachine vm(&reg);

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };
  auto callOn = [&](ObjectRef self, const char* type, const char* fn, std::vector<Value> args) {
    const NativeFunction* f = reg.Find(type, fn);
    return f ? (*f)(vm, self, args) : Value();
  };
  auto call = [&](const char* type, const char* fn, std::vector<Value> args) {
    return callOn(ObjectRef{0x14}, type, fn, args);
  };
  auto near = [](f32 a, f32 b) { return std::fabs(a - b) < 0.001f; };

  // Sun position: straight up at noon, straight down at midnight, unit length.
  bindings.game_time = 10.5f;  // noon on day 10
  check("sun Z up at noon", near(call("Game", "GetSunPositionZ", {}).ToFloat(), 1.0f));
  bindings.game_time = 10.0f;  // midnight
  check("sun Z down at midnight", near(call("Game", "GetSunPositionZ", {}).ToFloat(), -1.0f));
  bindings.game_time = 10.75f;  // 18:00
  {
    f32 x = call("Game", "GetSunPositionX", {}).ToFloat();
    f32 y = call("Game", "GetSunPositionY", {}).ToFloat();
    f32 z = call("Game", "GetSunPositionZ", {}).ToFloat();
    check("sun direction is unit length", near(std::sqrt(x * x + y * y + z * z), 1.0f));
  }

  // GameTimeToString formats the day and a 24-hour clock.
  check("GameTimeToString noon",
        call("Utility", "GameTimeToString", {Value::Float(12.5f)}).ToString() == "Day 12, 12:00");
  check("GameTimeToString quarter",
        call("Utility", "GameTimeToString", {Value::Float(3.25f)}).ToString() == "Day 3, 06:00");

  check("GetGameSettingInt truncates float",
        call("Game", "GetGameSettingInt", {Value::Str("fJumpHeightMin")}).ToInt() == 42);
  check("frame rate default", near(call("Utility", "GetAverageFrameRate", {}).ToFloat(), 60.0f));

  bindings.base_health = 150.0f;
  check("GetActorValueMax is base value",
        near(call("Actor", "GetActorValueMax", {Value::Str("Health")}).ToFloat(), 150.0f));

  // Container queries over a small mock inventory.
  check("empty container", call("ObjectReference", "IsContainerEmpty", {}).ToBool());
  bindings.inventory = {{ObjectRef{0x100}, 3}, {ObjectRef{0x200}, 5}};
  check("non-empty container", !call("ObjectReference", "IsContainerEmpty", {}).ToBool());
  check("GetAllItemsCount sums stacks",
        call("ObjectReference", "GetAllItemsCount", {}).ToInt() == 8);
  call("ObjectReference", "RemoveAllItems", {});
  check("RemoveAllItems empties inventory", bindings.inventory.empty());

  // FormList: base record entries plus a runtime addition.
  ObjectRef list{0x300}, formA{0x301}, formB{0x302}, runtime{0x303};
  bindings.list_forms = {formA, formB};
  auto on = [&](const char* fn, std::vector<Value> args) { return callOn(list, "FormList", fn, args); };
  check("FormList base size", on("GetSize", {}).ToInt() == 2);
  check("FormList base HasForm", on("HasForm", {Value::Object(formA)}).ToBool());
  check("FormList GetAt reads record", on("GetAt", {Value::Int(1)}).as_object().handle == formB.handle);
  check("FormList Find returns index", on("Find", {Value::Object(formB)}).ToInt() == 1);
  on("AddForm", {Value::Object(runtime)});
  check("FormList size includes runtime add", on("GetSize", {}).ToInt() == 3);
  check("FormList HasForm sees runtime add", on("HasForm", {Value::Object(runtime)}).ToBool());

  // State round-trips for the new stateful natives.
  ObjectRef npc{0x400};
  callOn(npc, "Actor", "SetGhost", {Value::Bool(true)});
  check("Actor ghost flag round-trips", callOn(npc, "Actor", "IsGhost", {}).ToBool());
  callOn(npc, "ObjectReference", "SetAngle",
         {Value::Float(10.0f), Value::Float(20.0f), Value::Float(30.0f)});
  check("ObjectReference angle round-trips",
        near(callOn(npc, "ObjectReference", "GetAngleY", {}).ToFloat(), 20.0f));

  // Faction crime-gold split: non-violent is the total minus the violent share.
  ObjectRef faction{0x500};
  bindings.crime_gold = 100;
  callOn(faction, "Faction", "SetCrimeGoldViolent", {Value::Int(30)});
  check("crime gold violent round-trips",
        callOn(faction, "Faction", "GetCrimeGoldViolent", {}).ToInt() == 30);
  check("crime gold non-violent is total minus violent",
        callOn(faction, "Faction", "GetCrimeGoldNonViolent", {}).ToInt() == 70);

  // Spell hostility derives from a detrimental magic effect on the record.
  ObjectRef spell{0x600};
  bindings.effects = {{ObjectRef{0x601}, false}};
  check("restorative spell is not hostile", !callOn(spell, "Spell", "IsHostile", {}).ToBool());
  bindings.effects = {{ObjectRef{0x601}, false}, {ObjectRef{0x602}, true}};
  check("spell with a detrimental effect is hostile",
        callOn(spell, "Spell", "IsHostile", {}).ToBool());

  // Sound.Play forwards the sound form to the audio binding and returns the voice.
  ObjectRef soundForm{0x700};
  Value played = callOn(soundForm, "Sound", "Play", {Value::Object(ObjectRef{0x701})});
  check("Sound.Play returns the voice id", played.ToInt() == 7);
  check("Sound.Play forwards the sound form", bindings.last_played.handle == soundForm.handle);

  // KeepOffsetFromActor / ClearKeepOffsetFromActor drive the follow wiring.
  ObjectRef follower{0x800};
  callOn(follower, "Actor", "KeepOffsetFromActor", {Value::Object(ObjectRef{0x14})});
  check("KeepOffsetFromActor starts following",
        bindings.following && bindings.follow_actor.handle == follower.handle);
  callOn(follower, "Actor", "ClearKeepOffsetFromActor", {});
  check("ClearKeepOffsetFromActor stops following", !bindings.following);

  // Debug.* engine commands route through the guest's command hook with a verb and
  // a string argument. The guest binds these in its constructor.
  {
    rec::script::PapyrusGuest guest(rec::bethesda::Game::kSkyrimSe);
    std::vector<std::pair<std::string, std::string>> cmds;
    guest.set_on_debug_command(
        [&](const std::string& verb, const std::string& arg) { cmds.emplace_back(verb, arg); });
    VirtualMachine gvm(&guest.natives());
    auto dbg = [&](const char* fn, std::vector<Value> a) {
      const NativeFunction* f = guest.natives().Find("Debug", fn);
      if (f) (*f)(gvm, ObjectRef{0x14}, a);
    };
    dbg("QuitGame", {});
    dbg("TakeScreenshot", {});
    dbg("ToggleMenus", {});
    dbg("SetGodMode", {Value::Bool(true)});
    check("QuitGame routes as a command", !cmds.empty() && cmds[0].first == "QuitGame");
    check("TakeScreenshot routes as a command", cmds.size() > 1 && cmds[1].first == "TakeScreenshot");
    check("ToggleMenus routes as a command", cmds.size() > 2 && cmds[2].first == "ToggleMenus");
    check("SetGodMode forwards its bool argument",
          cmds.size() > 3 && cmds[3].first == "SetGodMode" && cmds[3].second == "1");
  }

  std::printf("%s (%d failures)\n", failures ? "NATIVESEXTTEST FAILED" : "NATIVESEXTTEST PASSED",
              failures);
  return failures ? 1 : 0;
}

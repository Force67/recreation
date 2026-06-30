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

  f32 GetCurrentGameTime() override { return game_time; }
  f32 GetGameSettingFloat(const std::string&) override { return 42.7f; }
  f32 GetBaseActorValue(ObjectRef, const std::string&) override { return base_health; }
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
  auto call = [&](const char* type, const char* fn, std::vector<Value> args) {
    const NativeFunction* f = reg.Find(type, fn);
    return f ? (*f)(vm, ObjectRef{0x14}, args) : Value();
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

  std::printf("%s (%d failures)\n", failures ? "NATIVESEXTTEST FAILED" : "NATIVESEXTTEST PASSED",
              failures);
  return failures ? 1 : 0;
}

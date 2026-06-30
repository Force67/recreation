#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgB; using ext::ArgI; using ext::ArgO;
using ext::Resolve;
namespace st = state;

void RegisterItemsExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };

  // FormList runtime additions live in the shared member store, keyed by the list.
  reg.Register("FormList", "AddForm", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::AddMember(self, "list", ArgO(a, 0));
    return Value();
  });
  reg.Register("FormList", "RemoveAddedForm", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::RemoveMember(self, "list", ArgO(a, 0));
    return Value();
  });
  reg.Register("FormList", "HasForm", [](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(st::HasMember(self, "list", ArgO(a, 0)));
  });
  reg.Register("FormList", "GetSize", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(st::MemberCount(self, "list"));
  });
  // No wipe API on the member store, so Revert keeps the runtime additions.
  reg.Register("FormList", "Revert", noop);
  // GetAt/Find need the base FLST record read through a binding to be added later,
  // so they are placeholders until that ordered source exists.
  reg.Register("FormList", "GetAt", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Object(ObjectRef{});
  });
  reg.Register("FormList", "Find", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Int(-1);
  });

  // Leveled-list editing has no runtime store here, so these are no-ops.
  for (const char* type : {"LeveledActor", "LeveledItem", "LeveledSpell"}) {
    reg.Register(type, "AddForm", noop);
    reg.Register(type, "Revert", noop);
  }

  // Spell casting touches no subsystem yet.
  reg.Register("Spell", "Cast", noop);
  reg.Register("Spell", "RemoteCast", noop);
  reg.Register("Spell", "Preload", noop);
  reg.Register("Spell", "Unload", noop);
  // Hostility is not derivable from the available SPIT fields, so report false.
  reg.Register("Spell", "IsHostile", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });
  reg.Register("Scroll", "Cast", noop);
  reg.Register("Weapon", "Fire", noop);
  reg.Register("Enchantment", "IsHostile", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });
  reg.Register("Ingredient", "IsHostile", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });
  reg.Register("Potion", "IsHostile", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });

  // Ingredient effect-knowledge is not tracked, so these report learned.
  auto learned = [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(true); };
  reg.Register("Ingredient", "LearnEffect", learned);
  reg.Register("Ingredient", "LearnNextEffect", learned);
  reg.Register("Ingredient", "LearnAllEffects", learned);

  // The actor value the effect modifies is the closest available skill.
  reg.Register("MagicEffect", "GetAssociatedSkill", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Str(Resolve(bindings).GetMagicEffectActorValue(self));
  });

  reg.Register("Armor", "GetWarmthRating", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Float(0.0f);
  });
  reg.Register("Light", "GetWarmthRating", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Float(0.0f);
  });

  reg.Register("Keyword", "SendStoryEvent", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });
  reg.Register("Keyword", "SendStoryEventAndWait", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });

  reg.Register("Topic", "Add", noop);
  reg.Register("Scene", "IsActionComplete", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });
}

}  // namespace rec::script::skyrim

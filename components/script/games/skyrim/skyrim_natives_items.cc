#include "components/script/games/skyrim/skyrim_native_state.h"
#include "components/script/games/skyrim/skyrim_natives_ext.h"

namespace rx::script::skyrim {

using ext::ArgB;
using ext::ArgI;
using ext::ArgO;
using ext::Args;
using ext::Resolve;
using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
namespace st = state;

void RegisterItemsExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };

  // A form list is its authored FLST entries (read through the binding) plus any
  // forms a script added at runtime (kept in the shared member store, key "list").
  reg.Register("FormList", "AddForm", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::AddMember(self, "list", ArgO(a, 0));
    return Value();
  });
  reg.Register("FormList", "RemoveAddedForm", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::RemoveMember(self, "list", ArgO(a, 0));
    return Value();
  });
  reg.Register("FormList", "GetSize", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetFormListSize(self) + st::MemberCount(self, "list"));
  });
  reg.Register("FormList", "HasForm", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    ObjectRef form = ArgO(a, 0);
    if (st::HasMember(self, "list", form))
      return Value::Bool(true);
    auto& b = Resolve(bindings);
    for (i32 i = 0, n = b.GetFormListSize(self); i < n; ++i)
      if (b.GetNthListForm(i).handle == form.handle)
        return Value::Bool(true);
    return Value::Bool(false);
  });
  reg.Register("FormList", "GetAt", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    i32 index = ArgI(a, 0);
    i32 base = Resolve(bindings).GetFormListSize(self);
    if (index >= 0 && index < base)
      return Value::Object(Resolve(bindings).GetNthListForm(index));
    return Value::Object(ObjectRef{});  // a runtime-added form, which the set store cannot order
  });
  reg.Register("FormList", "Find", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    ObjectRef form = ArgO(a, 0);
    auto& b = Resolve(bindings);
    for (i32 i = 0, n = b.GetFormListSize(self); i < n; ++i)
      if (b.GetNthListForm(i).handle == form.handle)
        return Value::Int(i);
    return Value::Int(-1);
  });
  reg.Register("FormList", "Revert", noop);  // no wipe API, runtime additions stay

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
  reg.Register("Scroll", "Cast", noop);
  reg.Register("Weapon", "Fire", noop);

  // A spell, enchantment, or consumable is hostile when any of its magic effects
  // is detrimental, the same test the game uses to pick friend-or-foe magic. The
  // effects come from the item's record through the existing binding.
  auto hostile = [bindings](VirtualMachine&, ObjectRef self, Args&) {
    auto& b = Resolve(bindings);
    for (i32 i = 0, n = b.GetMagicEffectCount(self); i < n; ++i)
      if (b.GetMagicEffectDetrimental(b.GetNthMagicEffectId(i)))
        return Value::Bool(true);
    return Value::Bool(false);
  };
  reg.Register("Spell", "IsHostile", hostile);
  reg.Register("Enchantment", "IsHostile", hostile);
  reg.Register("Ingredient", "IsHostile", hostile);
  reg.Register("Potion", "IsHostile", hostile);

  // Effect knowledge, one bit per effect slot. An ingredient has four, and the
  // alchemy menu shows only the ones the player has discovered by eating it or
  // brewing with it. The natives return whether the call changed anything, so a
  // script can tell a new discovery from a repeat.
  constexpr i32 kAllEffects = 0x0f;
  reg.Register("Ingredient", "LearnEffect", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    auto& b = Resolve(bindings);
    const i32 slot = ArgI(a, 0);
    if (slot < 0 || slot > 3)
      return Value::Bool(false);
    const i32 known = b.GetKnownIngredientEffects(self);
    const i32 after = known | (1 << slot);
    b.SetKnownIngredientEffects(self, after);
    return Value::Bool(after != known);
  });
  reg.Register("Ingredient", "LearnNextEffect",
               [bindings](VirtualMachine&, ObjectRef self, Args&) {
                 auto& b = Resolve(bindings);
                 const i32 known = b.GetKnownIngredientEffects(self);
                 for (i32 slot = 0; slot < 4; ++slot) {
                   if (known & (1 << slot))
                     continue;
                   b.SetKnownIngredientEffects(self, known | (1 << slot));
                   return Value::Bool(true);
                 }
                 return Value::Bool(false);
               });
  reg.Register("Ingredient", "LearnAllEffects",
               [bindings](VirtualMachine&, ObjectRef self, Args&) {
                 auto& b = Resolve(bindings);
                 const i32 known = b.GetKnownIngredientEffects(self);
                 b.SetKnownIngredientEffects(self, kAllEffects);
                 return Value::Bool(known != kAllEffects);
               });
  reg.Register("Ingredient", "GetKnownEffectFlags",
               [bindings](VirtualMachine&, ObjectRef self, Args&) {
                 return Value::Int(Resolve(bindings).GetKnownIngredientEffects(self));
               });

  // The actor value the effect modifies is the closest available skill.
  reg.Register("MagicEffect", "GetAssociatedSkill",
               [bindings](VirtualMachine&, ObjectRef self, Args&) {
                 return Value::Str(Resolve(bindings).GetMagicEffectActorValue(self));
               });

  reg.Register("Armor", "GetWarmthRating",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Float(0.0f); });
  reg.Register("Light", "GetWarmthRating",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Float(0.0f); });

  reg.Register("Keyword", "SendStoryEvent",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Keyword", "SendStoryEventAndWait",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });

  reg.Register("Topic", "Add", noop);
  reg.Register("Scene", "IsActionComplete",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
}

}  // namespace rx::script::skyrim

#include "script/games/skyrim/skyrim_condition_context.h"

#include "script/games/skyrim/skyrim_bindings.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;

float SkyrimConditionContext::GetStage(u64 quest) const {
  return static_cast<float>(bindings_->GetStage(ObjectRef{quest}));
}

float SkyrimConditionContext::GetGlobal(u64 global) const {
  return bindings_->GetGlobalValue(ObjectRef{global});
}

float SkyrimConditionContext::GetItemCount(quest::RunOn run_on, u64 reference, u64 item) const {
  // Most dialogue item checks run on the player; an explicit reference run-on
  // names the container directly.
  const ObjectRef container =
      (run_on == quest::RunOn::kReference && reference) ? ObjectRef{reference} : bindings_->GetPlayer();
  return static_cast<float>(bindings_->GetItemCount(container, ObjectRef{item}));
}

bool SkyrimConditionContext::Supports(const quest::ConditionList& conditions) const {
  for (const quest::Comparison& c : conditions.comparisons) {
    switch (c.func) {
      case quest::Func::kGetStage:
      case quest::Func::kGetItemCount:
        break;  // evaluated faithfully (global right-hand sides included)
      default:
        return false;  // GetActorValue / GetDistance / unmapped: not yet judged
    }
  }
  return true;
}

bool SkyrimConditionContext::Allows(const quest::ConditionList& conditions) const {
  if (!Supports(conditions)) return true;
  return quest::Evaluate(conditions, *this);
}

}  // namespace rec::script::skyrim

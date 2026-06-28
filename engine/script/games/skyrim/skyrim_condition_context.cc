#include "script/games/skyrim/skyrim_condition_context.h"

#include "script/games/skyrim/skyrim_bindings.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;

float SkyrimConditionContext::GetStage(u64 quest) const {
  return static_cast<float>(bindings_->GetStage(ObjectRef{quest}));
}

float SkyrimConditionContext::GetStageDone(u64 quest, u64 stage) const {
  return bindings_->GetStageDone(ObjectRef{quest}, static_cast<i32>(stage)) ? 1.0f : 0.0f;
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

bool SkyrimConditionContext::Understood(quest::Func func) {
  switch (func) {
    case quest::Func::kGetStage:
    case quest::Func::kGetStageDone:
    case quest::Func::kGetItemCount:
      return true;  // backed by real bindings state (global RHS handled too)
    default:
      return false;  // GetIsId (speaker gate), GetActorValue, GetDistance, raw
  }
}

bool SkyrimConditionContext::Supports(const quest::ConditionList& conditions) const {
  for (const quest::Comparison& c : conditions.comparisons)
    if (!Understood(c.func)) return false;
  return true;
}

bool SkyrimConditionContext::Allows(const quest::ConditionList& conditions) const {
  // AND of OR-groups (comparisons linked by or_next). A group is satisfied if any
  // disjunct passes OR any disjunct uses a function we cannot judge (treated as
  // satisfied); the line is hidden only when an entire group of understood
  // conditions fails. This keeps stale stage-gated lines out while never hiding
  // on an unknown check.
  const auto& cs = conditions.comparisons;
  size_t i = 0;
  while (i < cs.size()) {
    bool group_ok = false;
    size_t j = i;
    for (;; ++j) {
      const quest::Comparison& c = cs[j];
      if (!Understood(c.func) || quest::EvaluateOne(c, *this)) group_ok = true;
      if (!c.or_next || j + 1 >= cs.size()) break;
    }
    if (!group_ok) return false;
    i = j + 1;
  }
  return true;
}

}  // namespace rec::script::skyrim

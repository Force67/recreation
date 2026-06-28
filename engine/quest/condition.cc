#include "quest/condition.h"

namespace rec::quest {
namespace {

float LeftSide(const Comparison& c, const ConditionContext& ctx) {
  switch (c.func) {
    case Func::kGetDistance:
      return ctx.GetDistance(c.run_on, c.reference, c.param1);
    case Func::kGetActorValue:
      return ctx.GetActorValue(c.run_on, c.reference, c.param1);
    case Func::kGetItemCount:
      return ctx.GetItemCount(c.run_on, c.reference, c.param1);
    case Func::kGetStage:
      return ctx.GetStage(c.param1);
    case Func::kGetStageDone:
      return ctx.GetStageDone(c.param1, c.param2);
    case Func::kGetIsId:
      break;  // speaker gate, judged by the dialogue layer; raw here
    case Func::kRaw:
      break;
  }
  return ctx.EvalRaw(c);
}

bool Apply(CompareOp op, float lhs, float rhs) {
  switch (op) {
    case CompareOp::kEqual:
      return lhs == rhs;
    case CompareOp::kNotEqual:
      return lhs != rhs;
    case CompareOp::kGreater:
      return lhs > rhs;
    case CompareOp::kGreaterOrEqual:
      return lhs >= rhs;
    case CompareOp::kLess:
      return lhs < rhs;
    case CompareOp::kLessOrEqual:
      return lhs <= rhs;
  }
  return false;
}

}  // namespace

bool EvaluateOne(const Comparison& c, const ConditionContext& ctx) {
  const float lhs = LeftSide(c, ctx);
  const float rhs = c.global ? ctx.GetGlobal(c.global) : c.value;
  return Apply(c.op, lhs, rhs);
}

bool Evaluate(const ConditionList& conditions, const ConditionContext& ctx) {
  // OR-groups (runs linked by or_next) are ANDed together. OR each comparison
  // into the open group, and close the group on the first comparison whose
  // or_next is clear; a closed group that is false fails the whole list.
  bool group = false;
  bool open = false;
  for (const Comparison& c : conditions.comparisons) {
    group = group || EvaluateOne(c, ctx);
    open = true;
    if (!c.or_next) {
      if (!group) return false;
      group = false;
      open = false;
    }
  }
  // A trailing group left open by an or_next set on the final comparison
  // (malformed data) still has to hold.
  if (open && !group) return false;
  return true;
}

}  // namespace rec::quest

#ifndef RECREATION_QUEST_CONDITION_H_
#define RECREATION_QUEST_CONDITION_H_

#include <vector>

#include "core/types.h"

namespace rx::quest {

// Native, engine-side representation of a Bethesda condition list (a run of CTDA
// subrecords). Quests gate stage/objective transitions on these and dialogue
// gates INFO responses on them. The IR is game-agnostic: ParseCtda (ctda.h)
// transpiles Skyrim CTDA into it, but the evaluator and the data shape know
// nothing about the record format, so a native authoring front-end can emit the
// same structs directly.

// Comparison operator, matching the top 3 bits of the CTDA operator byte.
enum class CompareOp : u8 {
  kEqual,
  kNotEqual,
  kGreater,
  kGreaterOrEqual,
  kLess,
  kLessOrEqual,
};

// Where a function's "self" is resolved before it runs (CTDA run-on type). The
// reference field's meaning depends on this: a form id for kReference, a quest
// alias index for kQuestAlias, and so on.
enum class RunOn : u8 {
  kSubject,
  kTarget,
  kReference,
  kCombatTarget,
  kLinkedRef,
  kQuestAlias,
  kPackageData,
  kEventData,
};

// Condition functions we understand well enough to dispatch to a typed
// ConditionContext query. Everything else stays kRaw and is handed to the
// context verbatim, so evaluator correctness never depends on this table being
// complete, it is an intentionally partial, high-confidence subset of the
// Creation Kit's condition-function enum, extended as coverage is needed.
enum class Func : u16 {
  kRaw = 0,       // not in the table; raw_function carries the original CK id
  kGetDistance,   // CK 1: distance from the run-on ref to ref param1
  kGetActorValue, // CK 14: actor value param1 of the run-on ref
  kGetItemCount,  // CK 47: count of item param1 held by the run-on ref
  kGetStage,      // CK 58: current journal stage of quest param1
  kGetStageDone,  // CK 59: whether stage param2 of quest param1 has been set
  kGetIsId,       // CK 72: the run-on ref's base form equals param1
};

// One CTDA comparison: function(params...) <op> value.
//
// Form-id-bearing fields (param1 for quest/item/target funcs, reference, global)
// hold the raw plugin-relative form id straight after ParseCtda; an engine pass
// (ResolveConditionForms) remaps them to packed GlobalFormId handles, which is
// why they are 64-bit. AV indices and other non-form params just keep their
// small value.
struct Comparison {
  Func func = Func::kRaw;
  u16 raw_function = 0;  // original CTDA function index, always set
  u64 param1 = 0;
  u64 param2 = 0;
  RunOn run_on = RunOn::kSubject;
  u64 reference = 0;  // form id, or alias index when run_on == kQuestAlias
  CompareOp op = CompareOp::kEqual;
  // Right-hand side: a literal float, unless `global` is non-zero, in which case
  // the value is the runtime value of that GLOB form (CTDA "use global" flag).
  float value = 0.0f;
  u64 global = 0;
  bool or_next = false;  // CTDA flag 0x01: OR this with the following comparison
};

// A condition list. Comparisons linked by or_next form OR-groups; the groups
// are ANDed together. An empty list is vacuously true.
struct ConditionList {
  std::vector<Comparison> comparisons;

  bool empty() const { return comparisons.empty(); }
};

// The world-state interface the evaluator queries. The engine implements this
// over its live game state; tests supply a mock. Functions outside Func land in
// EvalRaw, so the host can widen coverage without touching the evaluator.
class ConditionContext {
 public:
  virtual ~ConditionContext() = default;

  // Typed queries for the functions in Func. run_on/reference identify the
  // object the function runs against; params are the CTDA parameters (form-id
  // params are packed handles once resolved).
  virtual float GetDistance(RunOn run_on, u64 reference, u64 target) const { return 0.0f; }
  virtual float GetActorValue(RunOn run_on, u64 reference, u64 actor_value) const { return 0.0f; }
  virtual float GetItemCount(RunOn run_on, u64 reference, u64 item) const { return 0.0f; }
  virtual float GetStage(u64 quest) const { return 0.0f; }
  // 1 if stage `stage` of `quest` has been set (run), else 0.
  virtual float GetStageDone(u64 quest, u64 stage) const { return 0.0f; }

  // Runtime value of a GLOB form, for CTDA "use global" comparisons.
  virtual float GetGlobal(u64 global) const { return 0.0f; }

  // Any function not in Func. Return the function's numeric result; the
  // evaluator applies the comparison. The default 0 is the conservative
  // "not satisfied" for the common equality/threshold checks.
  virtual float EvalRaw(const Comparison& c) const { return 0.0f; }
};

// Evaluates the grouped AND-of-ORs against the context. Empty list -> true.
bool Evaluate(const ConditionList& conditions, const ConditionContext& ctx);

// Evaluates a single comparison: resolves its left side via the context and
// applies its operator against the right side (literal or global).
bool EvaluateOne(const Comparison& c, const ConditionContext& ctx);

}  // namespace rx::quest

#endif  // RECREATION_QUEST_CONDITION_H_

#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_CONDITION_CONTEXT_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_CONDITION_CONTEXT_H_

#include "core/types.h"
#include "quest/condition.h"

namespace rec::script::skyrim {

class RecordBackedSkyrimBindings;

// A quest::ConditionContext backed by the live Skyrim bindings, so CTDA-derived
// conditions (a dialogue INFO gate, say) evaluate against real engine state.
//
// It faithfully handles the functions whose meaning is unambiguous without a
// dialogue-specific subject: GetStage, GetStageDone, the "use global" right-hand
// side, and GetItemCount (defaulting the container to the player). Functions it
// cannot yet judge, GetActorValue (needs the AV index->name map), GetDistance
// (needs the run-on subject), anything unmapped, are treated as "pass" so a
// line is hidden only when a condition we DO understand fails (this is what keeps
// stale stage-gated chatter out of the menu while never hiding on an unknown
// check).
class SkyrimConditionContext : public quest::ConditionContext {
 public:
  explicit SkyrimConditionContext(RecordBackedSkyrimBindings* bindings) : bindings_(bindings) {}

  float GetStage(u64 quest) const override;
  float GetStageDone(u64 quest, u64 stage) const override;
  float GetGlobal(u64 global) const override;
  float GetItemCount(quest::RunOn run_on, u64 reference, u64 item) const override;

  // True if every comparison uses a function this context evaluates faithfully.
  bool Supports(const quest::ConditionList& conditions) const;

  // Availability: AND-of-OR-groups, where a comparison whose function we cannot
  // judge counts as satisfied, so a line is hidden only when an OR-group of
  // understood conditions all fail.
  bool Allows(const quest::ConditionList& conditions) const;

 private:
  // Whether this context evaluates `func` against real state (vs. treating it as
  // an unknown "pass").
  static bool Understood(quest::Func func);

  RecordBackedSkyrimBindings* bindings_;
};

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_CONDITION_CONTEXT_H_

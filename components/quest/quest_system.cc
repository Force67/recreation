#include "components/quest/quest_system.h"

#include <base/containers/set.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/to_string.h>

namespace rx::quest {

void QuestSystem::SetDefinition(QuestDef def) {
  const QuestHandle handle = def.handle;
  defs_[handle] = base::move(def);
}

const QuestDef* QuestSystem::Definition(QuestHandle handle) const {
  auto* it = defs_.find(handle);
  return it == nullptr ? nullptr : &*it;
}

const QuestSystem::QuestState* QuestSystem::Peek(QuestHandle quest) const {
  auto* it = states_.find(quest);
  return it == nullptr ? nullptr : &*it;
}

void QuestSystem::Notify(QuestHandle quest, QuestEvent event) {
  ++revision_;
  if (auto* it = states_.find(quest); it != nullptr)
    it->revision = revision_;
  for (auto& listener : listeners_)
    listener(quest, event);
}

i32 QuestSystem::GetStage(QuestHandle quest) const {
  const QuestState* s = Peek(quest);
  return s ? s->stage : 0;
}

bool QuestSystem::SetStage(QuestHandle quest, i32 stage) {
  QuestState& q = Mutable(quest);
  const bool already_done = q.stage_done[stage];
  q.stage = stage;
  q.stage_done[stage] = true;
  q.running = true;  // setting a stage implies the quest is running
  // Re-setting an already-done stage is a no-op in the game, so it raises no
  // event (and the caller does not re-run its fragment).
  if (!already_done)
    Notify(quest, QuestEvent::kStageChanged);
  return !already_done;
}

bool QuestSystem::GetStageDone(QuestHandle quest, i32 stage) const {
  const QuestState* s = Peek(quest);
  if (!s)
    return false;
  auto* it = s->stage_done.find(stage);
  return it != nullptr && *it;
}

bool QuestSystem::IsRunning(QuestHandle quest) const {
  const QuestState* s = Peek(quest);
  return s && s->running;
}

void QuestSystem::StartQuest(QuestHandle quest) {
  Mutable(quest).running = true;
  Notify(quest, QuestEvent::kStarted);
}

void QuestSystem::StopQuest(QuestHandle quest) {
  Mutable(quest).running = false;
  Notify(quest, QuestEvent::kStopped);
}

void QuestSystem::ResetQuest(QuestHandle quest) {
  states_[quest] = QuestState{};
  Notify(quest, QuestEvent::kReset);
}

bool QuestSystem::IsActive(QuestHandle quest) const {
  const QuestState* s = Peek(quest);
  return s ? s->active : true;
}

void QuestSystem::SetActive(QuestHandle quest, bool active) {
  Mutable(quest).active = active;
  Notify(quest, QuestEvent::kActiveChanged);
}

void QuestSystem::SetObjectiveDisplayed(QuestHandle quest, i32 objective, bool displayed) {
  Mutable(quest).objective_displayed[objective] = displayed;
  Notify(quest, QuestEvent::kObjectiveChanged);
}

void QuestSystem::SetObjectiveCompleted(QuestHandle quest, i32 objective, bool completed) {
  Mutable(quest).objective_completed[objective] = completed;
  Notify(quest, QuestEvent::kObjectiveChanged);
}

bool QuestSystem::IsObjectiveDisplayed(QuestHandle quest, i32 objective) const {
  const QuestState* s = Peek(quest);
  if (!s)
    return false;
  auto* it = s->objective_displayed.find(objective);
  return it != nullptr && *it;
}

bool QuestSystem::IsObjectiveCompleted(QuestHandle quest, i32 objective) const {
  const QuestState* s = Peek(quest);
  if (!s)
    return false;
  auto* it = s->objective_completed.find(objective);
  return it != nullptr && *it;
}

bool QuestSystem::IsComplete(QuestHandle quest) const {
  const QuestState* s = Peek(quest);
  if (!s)
    return false;
  if (s->complete_override)
    return true;  // mirrored from a remote authority
  const QuestDef* def = Definition(quest);
  if (!def)
    return false;
  // Scan the definition's completing stages rather than FindStage(current),
  // since a stage index can carry several log entries and only one need flag
  // completion.
  for (const StageDef& sd : def->stages) {
    if (!sd.complete_quest)
      continue;
    auto* it = s->stage_done.find(sd.index);
    if (it != nullptr && *it)
      return true;
  }
  return false;
}

void QuestSystem::FillStatus(QuestHandle quest, const QuestState& state, QuestStatus* out) const {
  const QuestDef* def = Definition(quest);
  out->handle = quest;
  out->running = state.running;
  out->active = state.active;
  out->stage = state.stage;
  out->complete = IsComplete(quest);
  out->revision = state.revision;
  if (def) {
    out->editor_id = def->editor_id;
    out->name = !def->name.empty() ? def->name : def->editor_id;
    if (const StageDef* sd = def->FindStage(state.stage))
      out->log_entry = sd->log_entry;
  }
  if (out->name.empty())
    out->name = base::ToString(quest);

  // Objectives in definition order, overlaid with live displayed/completed
  // bits. Any objective the script touched without a definition entry follows.
  base::Set<i32> seen;
  if (def) {
    for (const ObjectiveDef& od : def->objectives) {
      ObjectiveStatus os;
      os.index = od.index;
      os.text = od.text;
      if (auto* it = state.objective_displayed.find(od.index); it != nullptr)
        os.displayed = *it;
      if (auto* it = state.objective_completed.find(od.index); it != nullptr)
        os.completed = *it;
      out->objectives.push_back(base::move(os));
      seen.insert(od.index);
    }
  }
  // Objectives the script touched without a definition entry, from either map
  // (an objective can be completed without ever having been displayed).
  base::Set<i32> extras;
  for (const auto& [index, _] : state.objective_displayed)
    extras.insert(index);
  for (const auto& [index, _] : state.objective_completed)
    extras.insert(index);
  for (i32 index : extras) {
    if (seen.count(index))
      continue;
    ObjectiveStatus os;
    os.index = index;
    if (auto* it = state.objective_displayed.find(index); it != nullptr)
      os.displayed = *it;
    if (auto* it = state.objective_completed.find(index); it != nullptr)
      os.completed = *it;
    out->objectives.push_back(base::move(os));
    seen.insert(index);
  }
}

QuestStatus QuestSystem::Status(QuestHandle quest) const {
  QuestStatus out;
  static const QuestState kEmpty;
  const QuestState* s = Peek(quest);
  FillStatus(quest, s ? *s : kEmpty, &out);
  return out;
}

base::Vector<QuestStatus> QuestSystem::AllStatuses() const {
  base::Vector<QuestStatus> out;
  out.reserve(states_.size());
  for (const auto& [handle, state] : states_) {
    QuestStatus status;
    FillStatus(handle, state, &status);
    out.push_back(base::move(status));
  }
  return out;
}

base::Vector<QuestStatus> QuestSystem::RunningStatuses(bool include_inactive) const {
  base::Vector<QuestStatus> out;
  for (const auto& [handle, state] : states_) {
    if (!state.running)
      continue;
    if (!state.active && !include_inactive)
      continue;
    QuestStatus status;
    FillStatus(handle, state, &status);
    out.push_back(base::move(status));
  }
  return out;
}

void QuestSystem::ApplyStatus(const QuestStatus& status) {
  QuestState& q = Mutable(status.handle);
  q.running = status.running;
  q.active = status.active;
  q.stage = status.stage;
  q.complete_override = status.complete;
  q.stage_done[status.stage] = true;
  for (const ObjectiveStatus& os : status.objectives) {
    q.objective_displayed[os.index] = os.displayed;
    q.objective_completed[os.index] = os.completed;
  }
  Notify(status.handle, QuestEvent::kApplied);
}

}  // namespace rx::quest

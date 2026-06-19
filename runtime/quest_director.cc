#include "quest_director.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "actor_system.h"
#include "bethesda/script_attachment.h"
#include "core/log.h"
#include "core/math.h"
#include "engine_internal.h"
#include "interaction_system.h"
#include "npc_director.h"
#include "script/papyrus/value.h"
#include "world/components.h"
#include "world/objective_marker.h"

namespace rec {

QuestDirector::QuestDirector(EngineContext& ctx, ActorSystem* actors)
    : ctx_(ctx),
      actors_(actors),
      config_(*ctx.config),
      world_(*ctx.world),
      records_(*ctx.records),
      strings_(*ctx.strings),
      dialogue_(*ctx.dialogue),
      camera_(*ctx.camera),
      debug_ui_(*ctx.debug_ui),
      game_ui_(*ctx.game_ui) {}

u64 QuestDirector::FindQuestHandle(const std::string& edid) const {
  for (const auto& [handle, name] : quest_records_)
    if (name == edid) return handle;
  return 0;
}

void QuestDirector::PinJournalSlot(int i) {
  if (i >= 0 && i < static_cast<int>(journal_handles_.size())) pinned_quest_ = journal_handles_[i];
}

void QuestDirector::AttachQuestScripts() {
  if (!ctx_.scripts) return;
  // Quests are the game's always-on scripts. Every quest with a Papyrus script
  // is instantiated so the quest browser lists the full set (main quests
  // included), not an arbitrary prefix. config.max_quest_scripts > 0 caps it for
  // a faster bring-up; 0 (the default) attaches them all.
  int limit = config_.max_quest_scripts;
  int quests = 0;
  int instances = 0;
  records_.EachOfType(FourCc('Q', 'U', 'S', 'T'),
                      [&](bethesda::GlobalFormId id,
                          const bethesda::RecordStore::StoredRecord& stored) {
                        if (limit > 0 && quests >= limit) return;
                        bethesda::Record record;
                        if (!records_.Parse(id, &record)) return;
                        const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
                        if (!vmad) return;
                        bethesda::ScriptAttachment attachment;
                        std::vector<bethesda::QuestStageFragment> fragments;
                        if (!bethesda::ParseQuestFragments(vmad->data, &attachment, &fragments) ||
                            attachment.scripts.empty())
                          return;
                        u64 handle = static_cast<u64>(id.plugin) << 32 | id.local_id;
                        auto created = ctx_.scripts->AttachScripts(handle, attachment);
                        if (!created.empty()) {
                          ++quests;
                          instances += static_cast<int>(created.size());
                          // Parse the quest's stages and objectives (log text,
                          // objective text, compass targets) for the HUD/debugger.
                          quest::QuestDef def =
                              quest::ParseQuestDefinition(handle, record, &strings_);
                          // Resolve its objective compass targets from forced-ref
                          // aliases now, while the records are at hand.
                          IndexObjectiveTargets(def, stored.winning_plugin);
                          // Key the record list by editor id: it is the stable
                          // handle REC_START_QUEST and the debugger match on. The
                          // panel's display name comes from the quest definition.
                          std::string edid =
                              !def.editor_id.empty() ? def.editor_id : std::to_string(id.local_id);
                          quest_records_.push_back({handle, std::move(edid)});
                          // Register the stage->fragment map and definition on the
                          // guest thread (the bindings' only caller) so SetStage runs
                          // the quest's authored logic and snapshots carry its text.
                          auto* binds = ctx_.bindings;
                          ctx_.scripts->guest().Submit(
                              [binds, handle, def = std::move(def),
                               fragments = std::move(fragments)](
                                  rec::script::papyrus::VirtualMachine&) mutable {
                                binds->quest_system().SetDefinition(std::move(def));
                                for (const auto& f : fragments)
                                  binds->SetStageFragment(handle, f.stage, f.function);
                              });
                        }
                      });
  REC_INFO("papyrus: instantiated {} scripts across {} quests, {} script types loaded", instances,
           quests, ctx_.scripts->loaded_script_count());
  REC_INFO("quest: resolved {} objective compass targets from forced-ref aliases",
           objective_targets_.size());

  // REC_START_QUEST=<EDID>[:<stage>] starts a quest at load (runs its opening
  // stage fragment) so quest logic can be exercised without the UI. The optional
  // :stage drives it to that stage, e.g. REC_START_QUEST=MQ101:160 to surface an
  // objective on the HUD.
  if (const char* want = std::getenv("REC_START_QUEST")) {
    std::string spec = want;
    std::string edid = spec;
    i32 start_stage = -1;
    if (size_t colon = spec.find(':'); colon != std::string::npos) {
      edid = spec.substr(0, colon);
      start_stage = std::atoi(spec.c_str() + colon + 1);
    }
    auto* binds = ctx_.bindings;
    int started = 0;
    for (const auto& [handle, name] : quest_records_) {
      if (edid != "all" && name != edid) continue;
      ctx_.scripts->guest().Submit([binds, h = handle, start_stage](rec::script::papyrus::VirtualMachine&) {
        rec::script::papyrus::ObjectRef ref{h};
        binds->StartQuest(ref);
        if (start_stage >= 0) binds->SetStage(ref, start_stage);
      });
      // Open the debugger on the started quest so its stages/objectives show.
      if (edid != "all") quest_panel_.selected = handle;
      ++started;
      if (edid != "all") break;
    }
    REC_INFO("debug: started {} quest(s) matching '{}'", started, edid);
  }

  // The scripted MQ101 playthroughs run host-authoritatively: they drive quest
  // stages and steer NPCs, which a multiplayer client only mirrors via quest /
  // actor replication. Arming them on a client would push stage changes the
  // guest discards in replica mode and steer NPCs the host already owns.
  const bool host = ctx_.config->connect_address.empty();

  // REC_MQ101_DEMO seeds a playable slice of the first main quest: start MQ101
  // (its opening fragment surfaces the first objective), then the npc director
  // drops a waypoint and recruits followers once the player and that objective
  // exist. Walk to the marker to complete the quest.
  if (host && std::getenv("REC_MQ101_DEMO")) {
    const u64 handle = FindQuestHandle("MQ101");
    if (handle != 0) {
      quest_panel_.selected = handle;
      npc_->ArmMq101Demo(handle);
    }
    REC_INFO("debug: MQ101 breadcrumb demo armed (walk the waypoints to complete the quest)");
  }

  // REC_MQ101_SCENE arms an NPC-driven escort: once the player and NPCs exist,
  // a guide NPC leads the player along a path while MQ101 advances to completion.
  if (host && std::getenv("REC_MQ101_SCENE")) {
    npc_->ArmMq101Scene();
    REC_INFO("debug: MQ101 escort scene armed (a guide NPC will lead the player out)");
  }

  // REC_JOURNAL opens the quest journal at load (it is normally toggled with J),
  // for screenshots (cf. RECREATION_UI_MENU / REC_HIDE_DEBUG_UI).
  if (std::getenv("REC_JOURNAL")) journal_open_ = true;

}

void QuestDirector::ReportDialogue(const std::string& edid) {
  u64 handle = 0;
  for (const auto& [h, name] : quest_records_) {
    if (name == edid) {
      handle = h;
      break;
    }
  }
  if (handle == 0) {
    std::printf("dialogue report: no quest matching '%s'\n", edid.c_str());
    return;
  }
  const std::vector<dialogue::Handle>& topics = dialogue_.TopicsForQuest(handle);
  std::printf("=== dialogue for %s (0x%llx): %zu topics ===\n", edid.c_str(),
              static_cast<unsigned long long>(handle), topics.size());
  // Attaches an INFO's TIF_ script and calls its begin fragment, returning
  // whether the fragment actually dispatched (script loaded + function found) --
  // the end-to-end check that dialogue selection can advance quests.
  auto fire = [&](u64 info) -> bool {
    if (!ctx_.scripts || info == 0) return false;
    bethesda::GlobalFormId id{static_cast<u16>(info >> 32), static_cast<u32>(info & 0xffffffffu)};
    bethesda::Record rec;
    if (!records_.Parse(id, &rec)) return false;
    const bethesda::Subrecord* vmad = rec.Find(FourCc('V', 'M', 'A', 'D'));
    if (!vmad) return false;
    bethesda::ScriptAttachment att;
    bethesda::InfoFragments frags;
    if (!bethesda::ParseInfoFragments(vmad->data, &att, &frags) || frags.begin.function.empty())
      return false;
    ctx_.scripts->AttachScripts(info, att);
    std::string fn = frags.begin.function;
    return ctx_.scripts->guest()
        .SubmitFor([info, fn](script::papyrus::VirtualMachine& vm) {
          return vm.TryCall(script::papyrus::ObjectRef{info}, fn, {});
        })
        .get();
  };

  int with_fragment = 0;
  int with_conditions = 0;
  int dispatched = 0;
  for (dialogue::Handle t : topics) {
    bethesda::GlobalFormId dial{static_cast<u16>(t >> 32), static_cast<u32>(t & 0xffffffffu)};
    dialogue::Topic topic = dialogue::ParseTopic(records_, dial, &strings_);
    std::printf("topic [%s] \"%s\" (%zu responses)\n", topic.editor_id.c_str(),
                topic.text.c_str(), topic.responses.size());
    for (const dialogue::Response& r : topic.responses) {
      std::printf("  player: \"%s\"\n  npc:    \"%s\"\n", r.player_line.c_str(),
                  r.npc_line.c_str());
      if (!r.fragment_script.empty()) {
        ++with_fragment;
        const bool ran = fire(r.info);
        if (ran) ++dispatched;
        std::printf("  fragment: %s.%s [%s]\n", r.fragment_script.c_str(),
                    r.fragment_function.c_str(), ran ? "dispatched" : "no-op");
      }
      if (!r.conditions.empty()) {
        ++with_conditions;
        std::printf("  conditions: %zu\n", r.conditions.comparisons.size());
      }
    }
  }
  std::printf("=== %d responses carry a fragment (%d dispatched), %d carry conditions ===\n",
              with_fragment, dispatched, with_conditions);
  std::fflush(stdout);
}

void QuestDirector::ReportQuestToCompletion(const std::string& edid) {
  u64 handle = 0;
  for (const auto& [h, name] : quest_records_) {
    if (name == edid) {
      handle = h;
      break;
    }
  }
  if (handle == 0) {
    std::printf("quest report: no quest matching '%s'\n", edid.c_str());
    return;
  }

  auto* binds = ctx_.bindings;
  // Drive and snapshot on the guest thread (the bindings' only legal caller);
  // build the human-readable report there and print it on return.
  std::string report =
      ctx_.scripts->guest()
          .SubmitFor([binds, handle](rec::script::papyrus::VirtualMachine&) {
            using rec::script::papyrus::ObjectRef;
            quest::QuestSystem& qs = binds->quest_system();
            const ObjectRef ref{handle};
            std::string r;
            auto emit = [&](const std::string& line) {
              r += line;
              r += '\n';
            };

            const quest::QuestDef* def = qs.Definition(handle);
            emit(Fmt("=== quest report: %s (0x%llx) ===", def ? def->editor_id.c_str() : "?",
                     static_cast<unsigned long long>(handle)));
            if (!def) {
              emit("no definition parsed");
              return r;
            }
            emit(Fmt("name: %s", def->name.empty() ? "(none)" : def->name.c_str()));
            emit(Fmt("priority %d, %zu stages, %zu objectives, completion stage %d", def->priority,
                     def->stages.size(), def->objectives.size(), def->CompletionStage()));
            for (const quest::StageDef& s : def->stages)
              emit(Fmt("  stage %d%s %s", s.index, s.complete_quest ? " [completes]" : "",
                       s.log_entry.c_str()));
            for (const quest::ObjectiveDef& o : def->objectives)
              emit(Fmt("  objective %d: %s", o.index, o.text.c_str()));

            emit("driving to completion:");
            binds->StartQuest(ref);
            emit(Fmt("  start -> running=%d stage=%d", qs.IsRunning(handle), qs.GetStage(handle)));
            // Walk the defined stages in ascending order; each SetStage runs the
            // stage's authored fragment (objectives, ref enables, chained stages).
            std::vector<i32> order;
            for (const quest::StageDef& s : def->stages) order.push_back(s.index);
            std::sort(order.begin(), order.end());
            order.erase(std::unique(order.begin(), order.end()), order.end());
            for (i32 stage : order) {
              binds->SetStage(ref, stage);
              std::string shown;
              for (const quest::ObjectiveDef& o : def->objectives)
                if (qs.IsObjectiveDisplayed(handle, o.index))
                  shown += Fmt(" [%d]", o.index);
              emit(Fmt("  set stage %d -> stage=%d complete=%d displayed:%s", stage,
                       qs.GetStage(handle), qs.IsComplete(handle),
                       shown.empty() ? " none" : shown.c_str()));
            }
            const i32 cs = def->CompletionStage();
            if (cs >= 0 && !qs.IsComplete(handle)) {
              binds->SetStage(ref, cs);
              emit(Fmt("  set completion stage %d -> complete=%d", cs, qs.IsComplete(handle)));
            }

            quest::QuestStatus st = qs.Status(handle);
            emit(Fmt("result: running=%d active=%d stage=%d complete=%s", st.running, st.active,
                     st.stage, st.complete ? "YES" : "no"));
            for (const quest::ObjectiveStatus& o : st.objectives)
              emit(Fmt("  objective %d: displayed=%d completed=%d  %s", o.index, o.displayed,
                       o.completed, o.text.c_str()));
            return r;
          })
          .get();
  std::printf("%s", report.c_str());
  std::fflush(stdout);
}

void QuestDirector::RefreshQuestPanel(f32 dt) {
  if (!ctx_.scripts || !ctx_.bindings || quest_records_.empty()) {
    quest_panel_.available = false;
    return;
  }
  quest_panel_.available = true;

  // Mutations run on the guest thread (the bindings' only legal caller).
  if (!quest_panel_.set_running) {
    quest_panel_.set_running = [this](u64 handle, bool run) {
#if RECREATION_HAS_NET
      // On a client, the debugger acts through the server (authoritative).
      if (ctx_.client_session && ctx_.client_session->joined()) {
        ctx_.client_session->SendStageRequest({handle, net::StageOp::kSetRunning, 0, run ? 1 : 0});
        return;
      }
#endif
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, handle, run](script::papyrus::VirtualMachine&) {
        if (run)
          binds->StartQuest(script::papyrus::ObjectRef{handle});
        else
          binds->StopQuest(script::papyrus::ObjectRef{handle});
      });
    };
    quest_panel_.set_stage = [this](u64 handle, i32 stage) {
#if RECREATION_HAS_NET
      if (ctx_.client_session && ctx_.client_session->joined()) {
        ctx_.client_session->SendStageRequest({handle, net::StageOp::kSetStage, stage, 0});
        return;
      }
#endif
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, handle, stage](script::papyrus::VirtualMachine&) {
        binds->SetStage(script::papyrus::ObjectRef{handle}, stage);
      });
    };
    quest_panel_.set_objective_displayed = [this](u64 handle, i32 objective, bool displayed) {
#if RECREATION_HAS_NET
      if (ctx_.client_session && ctx_.client_session->joined()) {
        ctx_.client_session->SendStageRequest(
            {handle, net::StageOp::kSetObjectiveDisplayed, objective, displayed ? 1 : 0});
        return;
      }
#endif
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, handle, objective, displayed](script::papyrus::VirtualMachine&) {
        binds->SetObjectiveDisplayed(script::papyrus::ObjectRef{handle}, objective, displayed);
      });
    };
    quest_panel_.set_objective_completed = [this](u64 handle, i32 objective, bool completed) {
#if RECREATION_HAS_NET
      if (ctx_.client_session && ctx_.client_session->joined()) {
        ctx_.client_session->SendStageRequest(
            {handle, net::StageOp::kSetObjectiveCompleted, objective, completed ? 1 : 0});
        return;
      }
#endif
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, handle, objective, completed](script::papyrus::VirtualMachine&) {
        binds->SetObjectiveCompleted(script::papyrus::ObjectRef{handle}, objective, completed);
      });
    };
    quest_panel_.set_follower = [this](u64 npc, bool follow) { npc_->SetFollower(npc, follow); };
    quest_panel_.place_marker = [this](u64 quest, i32 objective, i32 advance_stage) {
      QuestMarker m;
      m.quest = quest;
      m.objective = objective;
      m.advance_stage = advance_stage;
      Vec3 pp;
      if (actors_->PlayerWorldPos(&pp)) m.pos = pp;
      quest_markers_.push_back(m);
      REC_INFO("quest: placed marker for objective {} of 0x{:x} -> advance to stage {}", objective,
               quest, advance_stage);
    };
    quest_panel_.clear_markers = [this] { quest_markers_.clear(); };
  }

  // Snapshot the live state at a few Hz; one guest round-trip serves both the
  // debug panel (every quest, lightweight) and the HUD (only running quests,
  // with their objective text).
  quest_ui_timer_ -= dt;
  if (!quest_panel_.quests.empty() && quest_ui_timer_ > 0.0f) return;
  quest_ui_timer_ = 0.2f;
  auto* binds = ctx_.bindings;
  base::Vector<std::pair<u64, std::string>> src = quest_records_;
  u64 selected = quest_panel_.selected;

  struct Snapshot {
    std::vector<QuestPanel::Quest> panel;
    std::vector<quest::QuestStatus> running;
    QuestPanel::Detail detail;
  };
  Snapshot snap =
      ctx_.scripts->guest()
          .SubmitFor([binds, src, selected](script::papyrus::VirtualMachine&) {
            const quest::QuestSystem& qs = binds->quest_system();
            Snapshot out;
            out.panel.reserve(src.size());
            for (const auto& [handle, edid] : src) {
              const quest::QuestDef* def = qs.Definition(handle);
              std::string name = (def && !def->name.empty()) ? def->name : edid;
              out.panel.push_back({std::move(name), handle, qs.IsRunning(handle),
                                   qs.IsActive(handle), qs.IsComplete(handle), qs.GetStage(handle)});
            }
            out.running = qs.RunningStatuses();
            // Expand the selected quest into stages and objectives for the debugger.
            if (selected != 0) {
              out.detail.handle = selected;
              if (const quest::QuestDef* def = qs.Definition(selected)) {
                out.detail.editor_id = def->editor_id;
                out.detail.completion_stage = def->CompletionStage();
                for (const quest::StageDef& s : def->stages)
                  out.detail.stages.push_back({s.index, s.log_entry, qs.GetStageDone(selected, s.index)});
              }
              quest::QuestStatus st = qs.Status(selected);
              for (const quest::ObjectiveStatus& o : st.objectives)
                out.detail.objectives.push_back({o.index, o.text, o.displayed, o.completed});
            }
            return out;
          })
          .get();
  quest_panel_.quests = std::move(snap.panel);
  quest_panel_.detail = std::move(snap.detail);
  // Surface the look-target and counts so the debugger can toggle follow and
  // show how much is armed.
  const u64 look = interaction_->activate_target();
  quest_panel_.look_target = look;
  quest_panel_.look_label = interaction_->activate_label();
  quest_panel_.look_following = look != 0 && npc_->is_follower(look);
  quest_panel_.follower_count = npc_->follower_count();
  quest_panel_.marker_count = static_cast<int>(quest_markers_.size());
  UpdateQuestHud(snap.running);
  UpdateObjectiveMarkers(snap.running);
}

void QuestDirector::UpdateQuestHud(const std::vector<quest::QuestStatus>& running) {
  // The tracked quest is the player's pinned one (from the journal) if it is
  // still running, otherwise the most recently changed.
  const quest::QuestStatus* tracked = nullptr;
  const quest::QuestStatus* pinned = nullptr;
  for (const quest::QuestStatus& q : running) {
    if (!tracked || q.revision > tracked->revision) tracked = &q;
    if (pinned_quest_ != 0 && q.handle == pinned_quest_) pinned = &q;
  }
  if (pinned) tracked = pinned;

  // Player journal: the active quests, most recent first, capped to the HUD's
  // row pool; the tracked quest is the highlighted entry.
  std::vector<const quest::QuestStatus*> sorted;
  sorted.reserve(running.size());
  for (const quest::QuestStatus& q : running) sorted.push_back(&q);
  std::sort(sorted.begin(), sorted.end(),
            [](const quest::QuestStatus* a, const quest::QuestStatus* b) {
              return a->revision > b->revision;
            });
  std::vector<HudQuest> journal;
  journal_handles_.clear();
  int journal_selected = -1;
  for (const quest::QuestStatus* q : sorted) {
    if (journal.size() >= 6) break;
    HudQuest hq;
    hq.title = q->name;
    for (const quest::ObjectiveStatus& o : q->objectives)
      if (o.displayed || o.completed) hq.objectives.push_back({o.text, o.completed});
    if (tracked && q->handle == tracked->handle) journal_selected = static_cast<int>(journal.size());
    journal_handles_.push_back(q->handle);
    journal.push_back(std::move(hq));
  }
  game_ui_.SetJournal(journal_open_, journal, journal_selected);

  if (!tracked) {
    if (hud_tracked_quest_ != 0) {
      hud_tracked_quest_ = 0;
      hud_tracked_revision_ = 0;
      game_ui_.SetQuest(HudQuest{});
    }
    return;
  }

  HudQuest hud;
  hud.title = tracked->name;
  for (const quest::ObjectiveStatus& o : tracked->objectives) {
    if (!o.displayed && !o.completed) continue;
    hud.objectives.push_back({o.text, o.completed});
  }
  game_ui_.SetQuest(hud);

  // Raise the banner once per change: when the tracked quest switches or its
  // revision advances.
  if (tracked->handle != hud_tracked_quest_ || tracked->revision != hud_tracked_revision_) {
    if (hud_tracked_revision_ != 0 || tracked->handle != hud_tracked_quest_)
      game_ui_.FlashQuestUpdate(tracked->complete ? tracked->name + " (Complete)" : tracked->name);
    hud_tracked_quest_ = tracked->handle;
    hud_tracked_revision_ = tracked->revision;
  }
}

void QuestDirector::UpdateObjectiveMarkers(const std::vector<quest::QuestStatus>& running) {
  // A multiplayer client never owns markers or triggers; it shows the host's
  // replicated marker, driven from its own camera.
  if (ctx_.client_session) {
    DriveObjectiveMarkerHud(remote_marker_active_, remote_marker_pos_);
    return;
  }

  // A marker is armed when its own quest is running and its objective is the
  // current displayed-and-incomplete one. Checked per marker against its own
  // quest (not just the single most-recently-changed one), so a seeded marker
  // arms reliably even with many quests running at once. always_arm markers
  // (demo / scripted) manage their own life.
  auto is_armed = [&](const QuestMarker& m) -> bool {
    if (m.fired) return false;
    if (m.always_arm) return true;
    for (const quest::QuestStatus& q : running) {
      if (q.handle != m.quest) continue;
      for (const quest::ObjectiveStatus& o : q.objectives)
        if (o.index == m.objective) return o.displayed && !o.completed;
      return false;
    }
    return false;
  };
  QuestMarker* armed = nullptr;
  for (QuestMarker& m : quest_markers_)
    if (is_armed(m)) {
      armed = &m;
      break;
    }

  // Trigger: a player within an armed marker's radius advances the quest's stage
  // (host authoritative), evaluated against the local player plus every
  // networked one. A client receives the advance via quest replication.
  if (armed && armed->advance_stage >= 0 && ctx_.scripts && ctx_.bindings) {
    const float marker[3] = {armed->pos.x, armed->pos.y, armed->pos.z};
    bool reached = false;
    Vec3 pp;
    if (actors_->PlayerWorldPos(&pp)) {
      const float pp3[3] = {pp.x, pp.y, pp.z};
      reached = world::MarkerReached(pp3, marker, armed->radius);
    }
    if (!reached)
      world_.Each<net::NetworkId, world::Transform>(
          [&](ecs::Entity, net::NetworkId&, world::Transform& t) {
            if (world::MarkerReached(t.position, marker, armed->radius)) reached = true;
          });
    if (reached) {
      armed->fired = true;
      const u64 quest = armed->quest;
      const i32 stage = armed->advance_stage;
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, quest, stage](script::papyrus::VirtualMachine&) {
        binds->SetStage(script::papyrus::ObjectRef{quest}, stage);
      });
      REC_INFO("quest: reached objective {} marker, advancing 0x{:x} to stage {}", armed->objective,
               quest, stage);
    }
  }

  // With nothing armed, still guide the player: point the compass at the tracked
  // quest's current objective target (its forced-reference alias's world
  // position). This only drives the HUD pip, it never advances a stage.
  Vec3 guide_pos{};
  bool guide_active = false;
  // ObjectiveTargetFor only returns a target whose space matches the player's
  // current one (interior vs worldspace), so the bearing is always meaningful.
  if (!armed && hud_tracked_quest_ != 0) {
    for (const quest::QuestStatus& q : running) {
      if (q.handle != hud_tracked_quest_) continue;
      for (const quest::ObjectiveStatus& o : q.objectives) {
        if (!o.displayed || o.completed) continue;
        if (ObjectiveTargetFor(q.handle, o.index, &guide_pos)) {
          guide_active = true;
          break;
        }
      }
      break;
    }
  }

  const bool active = armed != nullptr || guide_active;
  const Vec3 pos = armed ? armed->pos : guide_pos;
  const u64 marker_quest = armed ? armed->quest : (guide_active ? hud_tracked_quest_ : 0);
  DriveObjectiveMarkerHud(active, pos);

#if RECREATION_HAS_NET
  // Replicate the active marker to clients, but only when it meaningfully
  // changes (the 0.1 m guard keeps float jitter off the reliable channel).
  if (ctx_.server_session) {
    const u64 quest = marker_quest;
    const bool changed =
        active != sent_marker_active_ || quest != sent_marker_quest_ ||
        (active && (std::fabs(pos.x - sent_marker_pos_.x) > 0.1f ||
                    std::fabs(pos.y - sent_marker_pos_.y) > 0.1f ||
                    std::fabs(pos.z - sent_marker_pos_.z) > 0.1f));
    if (changed) {
      net::ObjectiveMarkerState m;
      m.active = active;
      m.quest = quest;
      m.x = pos.x;
      m.y = pos.y;
      m.z = pos.z;
      ctx_.server_session->SendObjectiveMarker(m);
      sent_marker_active_ = active;
      sent_marker_pos_ = pos;
      sent_marker_quest_ = quest;
    }
  }
#endif
}

void QuestDirector::DriveObjectiveMarkerHud(bool active, const Vec3& pos) {
  if (config_.headless) return;
  if (!active || !actors_->HasPlayer()) {
    game_ui_.SetObjectiveMarker(false, 0, 0);
    return;
  }
  Vec3 ppos{};
  actors_->PlayerWorldPos(&ppos);
  const Vec3 view = ctx_.walk_mode ? (ctx_.walk_target - ctx_.walk_eye) : camera_.forward();
  const Vec3 to{pos.x - ppos.x, 0, pos.z - ppos.z};
  const float view_fwd[3] = {view.x, view.y, view.z};
  const float to_marker[3] = {to.x, to.y, to.z};
  const f32 distance = Length(to);
  const f32 bearing = world::MarkerCompassBearingDeg(view_fwd, to_marker);
  game_ui_.SetObjectiveMarker(true, bearing, distance);
}

// Packs a (quest handle, objective index) pair into one key. A quest handle
// uses at most 48 bits and objective indices are small, so 12 low bits hold the
// objective without colliding.
static u64 ObjectiveKey(u64 quest, i32 objective) {
  return (quest << 12) | (static_cast<u64>(objective) & 0xfffu);
}

bool QuestDirector::RefWorldPosition(bethesda::GlobalFormId ref, Vec3* out) const {
  bethesda::Record record;
  if (!records_.Parse(ref, &record)) return false;
  const bethesda::Subrecord* data = record.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 12) return false;
  f32 p[3];
  std::memcpy(p, data->data.data(), 12);
  constexpr f32 kUnitsToMeters = 0.01428f;  // Bethesda -> engine, axes (x, z, -y)
  *out = Vec3{p[0] * kUnitsToMeters, p[2] * kUnitsToMeters, -p[1] * kUnitsToMeters};
  return true;
}

void QuestDirector::IndexObjectiveTargets(const quest::QuestDef& def, u16 plugin) {
  for (const quest::ObjectiveDef& obj : def.objectives) {
    for (i32 alias_id : obj.target_aliases) {
      const quest::AliasDef* alias = def.FindAlias(alias_id);
      if (!alias || alias->forced_ref_raw == 0) continue;
      bethesda::GlobalFormId ref =
          records_.ResolveFrom(bethesda::RawFormId{alias->forced_ref_raw}, plugin);
      // Interior and exterior targets are both kept; the space is recorded so the
      // compass only points at a target the player can actually navigate toward.
      const bool interior = records_.InteriorCellOfRef(ref).plugin != 0xffff;
      Vec3 pos;
      if (RefWorldPosition(ref, &pos)) {
        objective_targets_.insert(ObjectiveKey(def.handle, obj.index), ObjTarget{pos, interior});
        break;  // first resolvable target alias wins
      }
    }
  }
}

bool QuestDirector::ObjectiveTargetFor(u64 quest, i32 objective, Vec3* out) const {
  const ObjTarget* t = objective_targets_.find(ObjectiveKey(quest, objective));
  if (!t) return false;
  // A target's position is only in the player's coordinate space when both are
  // interior or both exterior; otherwise the bearing would point nowhere useful.
  const bool player_interior = ctx_.streamer && ctx_.streamer->in_interior();
  if (t->interior != player_interior) return false;
  *out = t->pos;
  return true;
}

void QuestDirector::RefreshNativeTrace(f32 dt) {
  if (!ctx_.scripts) {
    native_trace_panel_.available = false;
    return;
  }
  native_trace_panel_.available = true;
  if (!native_trace_panel_.clear) {
    native_trace_panel_.clear = [this] {
      ctx_.scripts->guest().Submit(
          [](script::papyrus::VirtualMachine& vm) { vm.ClearNativeTrace(); });
    };
  }

  // Tracing copies two strings per native call, so only run it while the window
  // is open; flip the guest's flag when the visibility changes.
  bool want = debug_ui_.trace_visible();
  if (want != native_trace_on_) {
    native_trace_on_ = want;
    ctx_.scripts->guest().Submit(
        [want](script::papyrus::VirtualMachine& vm) { vm.set_native_trace(want); });
  }
  if (!want) return;

  trace_ui_timer_ -= dt;
  if (!native_trace_panel_.recent.empty() && trace_ui_timer_ > 0.0f) return;
  trace_ui_timer_ = 0.15f;

  using NativeCall = script::papyrus::VirtualMachine::NativeCall;
  auto snap = ctx_.scripts->guest()
                  .SubmitFor([](script::papyrus::VirtualMachine& vm) {
                    return std::pair<u64, std::vector<NativeCall>>(vm.native_call_count(),
                                                                   vm.native_trace_log());
                  })
                  .get();
  native_trace_panel_.total = snap.first;
  const std::vector<NativeCall>& log = snap.second;

  native_trace_panel_.recent.clear();
  native_trace_panel_.recent.reserve(log.size());
  for (auto it = log.rbegin(); it != log.rend(); ++it)
    native_trace_panel_.recent.push_back(it->script_type + "." + it->function);

  std::unordered_map<std::string, u32> counts;
  for (const NativeCall& c : log) ++counts[c.script_type + "." + c.function];
  std::vector<std::pair<std::string, u32>> top(counts.begin(), counts.end());
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  if (top.size() > 40) top.resize(40);
  native_trace_panel_.top = std::move(top);
}

}  // namespace rec

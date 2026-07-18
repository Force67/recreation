#include "quest_director.h"

#include <base/option.h>

#include <algorithm>
#include <cctype>
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
#include "quest/scene_player.h"
#include "quest/scene_record.h"
#include "script/papyrus/alias_handle.h"
#include "script/papyrus/value.h"
#include "world/components.h"
#include "world/objective_marker.h"

namespace rx {

namespace {

void ResolveAttachmentForms(bethesda::RecordStore& records, u16 plugin,
                            bethesda::ScriptAttachment* attachment) {
  bethesda::ResolveScriptObjectForms(attachment, [&](u32 raw) {
    return records.ResolveFrom(bethesda::RawFormId{raw}, plugin).packed();
  });
}

// Config options (formerly RX_* env vars; populated by InitOptionsFromEnv()).
base::Option<bool> NoAutostart{"no.autostart", false, "RX_NO_AUTOSTART"};
base::Option<const char*> StartQuest{"start.quest", nullptr, "RX_START_QUEST"};
base::Option<bool> Mq101Demo{"mq101.demo", false, "RX_MQ101_DEMO"};
base::Option<bool> Mq101Scene{"mq101.scene", false, "RX_MQ101_SCENE"};
base::Option<bool> CwBattle{"cw.battle", false, "RX_CW_BATTLE"};
base::Option<bool> CwFieldBattle{"cw.field.battle", false, "RX_CW_FIELD_BATTLE"};
base::Option<bool> Cw00Demo{"cw00.demo", false, "RX_CW00_DEMO"};
base::Option<bool> CwDemo{"cw.demo", false, "RX_CW_DEMO"};
base::Option<bool> CwSiegeDemo{"cw.siege.demo", false, "RX_CW_SIEGE_DEMO"};
base::Option<const char*> CwSide{"cw.side", nullptr, "RX_CW_SIDE"};
base::Option<bool> Journal{"journal", false, "RX_JOURNAL"};

// One of a quest's scenes: its handle, editor id, and parsed Papyrus fragments.
struct SceneJob {
  u64 handle = 0;
  std::string edid;
  bethesda::SceneFragments frags;
};

// Parses every SCEN owned by `quest`, attaches its SF_ script so the VM can run
// the Fragment_N functions, and returns the jobs. Main-thread only: AttachScripts
// marshals to the guest, which would deadlock if called from it.
std::vector<SceneJob> GatherQuestScenes(bethesda::RecordStore& records,
                                        script::ScriptSystem* scripts, u64 quest) {
  std::vector<SceneJob> jobs;
  records.EachOfType(
      FourCc('S', 'C', 'E', 'N'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        bethesda::Record rec;
        if (!records.Parse(id, &rec)) return;
        quest::SceneDef def = quest::ParseSceneRecord(id.packed(), rec, &records);
        if (def.quest != quest && (def.quest & 0xffffffffu) != (quest & 0xffffffffu)) return;
        const bethesda::Subrecord* vmad = rec.Find(FourCc('V', 'M', 'A', 'D'));
        if (!vmad) return;
        bethesda::ScriptAttachment att;
        bethesda::SceneFragments frags;
        if (!bethesda::ParseSceneFragments(vmad->data, &att, &frags) || att.scripts.empty()) return;
        if (frags.begin.function.empty() && frags.end.function.empty() && frags.phases.empty())
          return;
        ResolveAttachmentForms(records, stored.winning_plugin, &att);
        scripts->AttachScripts(id.packed(), att);
        jobs.push_back({id.packed(), rec.GetString(FourCc('E', 'D', 'I', 'D')), std::move(frags)});
      });
  return jobs;
}

// Bridges ScenePlayer cues to the bindings' scene fragment runners (which run the
// SCEN's Papyrus fragments; those call Quest.SetStage to advance the journal).
struct SceneCueSink : quest::ScenePlayerSink {
  script::skyrim::RecordBackedSkyrimBindings* b;
  void SceneBegin(u64 scene) override { b->RunSceneBegin(scene); }
  void ScenePhase(u64 scene, u32 phase, bool on_begin) override {
    b->RunScenePhase(scene, phase, on_begin);
  }
  void SceneEnd(u64 scene) override { b->RunSceneEnd(scene); }
};

}  // namespace

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
  // Start-Game-Enabled quests come online at load (RX_NO_AUTOSTART disables it).
  const bool no_autostart_ = bool(NoAutostart);
  int autostarted = 0;
  records_.EachOfType(
      FourCc('Q', 'U', 'S', 'T'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        if (limit > 0 && quests >= limit) return;
        bethesda::Record record;
        if (!records_.Parse(id, &record)) return;
        const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
        if (!vmad) return;
        bethesda::ScriptAttachment attachment;
        std::vector<bethesda::QuestStageFragment> fragments;
        std::vector<bethesda::QuestAliasScripts> alias_scripts;
        if (!bethesda::ParseQuestFragments(vmad->data, &attachment, &fragments, &alias_scripts) ||
            attachment.scripts.empty())
          return;
        ResolveAttachmentForms(records_, stored.winning_plugin, &attachment);
        for (bethesda::QuestAliasScripts& alias : alias_scripts)
          ResolveAttachmentForms(records_, stored.winning_plugin, &alias.scripts);
        u64 handle = static_cast<u64>(id.plugin) << 32 | id.local_id;
        // Attaching the Papyrus scripts is best effort: the stage
        // machine, objectives and the debugger are driven by the
        // QUST record, not the bytecode. Starfield ships PEX the VM
        // does not yet execute, so its scripts fail to load; the
        // quest must still register so SetStage and the debugger
        // work. Only the stage fragments' side effects are lost.
        auto created = ctx_.scripts->AttachScripts(handle, attachment);
        ++quests;
        instances += static_cast<int>(created.size());
        // Attach the per-alias scripts on their alias handles, so
        // an alias's events (OnInit, and OnDeath once the alias is
        // filled) run, e.g. the Civil War reinforcement soldiers.
        for (const bethesda::QuestAliasScripts& a : alias_scripts) {
          const u64 alias_handle = script::papyrus::EncodeAliasHandle(handle, a.alias_id);
          instances +=
              static_cast<int>(ctx_.scripts->AttachScripts(alias_handle, a.scripts).size());
        }
        // Parse the quest's stages and objectives (log text,
        // objective text, compass targets) for the HUD/debugger.
        quest::QuestDef def = quest::ParseQuestDefinition(handle, record, &strings_);
        // Resolve its objective compass targets from forced-ref
        // aliases now, while the records are at hand.
        IndexObjectiveTargets(def, stored.winning_plugin);
        // Key the record list by editor id: it is the stable
        // handle RX_START_QUEST and the debugger match on. The
        // panel's display name comes from the quest definition.
        std::string edid = !def.editor_id.empty() ? def.editor_id : std::to_string(id.local_id);
        quest_records_.push_back({handle, std::move(edid)});
        // Register the stage->fragment map and definition on the
        // guest thread (the bindings' only caller) so SetStage runs
        // the quest's authored logic and snapshots carry its text.
        // Start-Game-Enabled quests (DNAM 0x01) are the game's
        // always-on controllers and intro quests, start them so
        // their dialogue topics and start logic come online, the
        // way the story manager would (e.g. CW00A's join lines).
        const bool sge = def.start_game_enabled && !no_autostart_;
        if (sge) ++autostarted;
        auto* binds = ctx_.bindings;
        ctx_.scripts->guest().Submit(
            [binds, handle, sge, def = std::move(def),
             fragments = std::move(fragments)](rx::script::papyrus::VirtualMachine&) mutable {
              binds->quest_system().SetDefinition(std::move(def));
              for (const auto& f : fragments) binds->SetStageFragment(handle, f.stage, f.function);
              if (sge) binds->StartQuest(rx::script::papyrus::ObjectRef{handle});
            });
      });
  RX_INFO("papyrus: instantiated {} scripts across {} quests, {} script types loaded", instances,
          quests, ctx_.scripts->loaded_script_count());
  RX_INFO("quest: auto-started {} start-game-enabled quests", autostarted);
  RX_INFO("quest: resolved {} objective compass targets from forced-ref aliases",
          objective_targets_.size());

  // RX_START_QUEST=<EDID>[:<stage>] starts a quest at load (runs its opening
  // stage fragment) so quest logic can be exercised without the UI. The optional
  // :stage drives it to that stage, e.g. RX_START_QUEST=MQ101:160 to surface an
  // objective on the HUD.
  if (const char* want = StartQuest.get()) {
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
      ctx_.scripts->guest().Submit(
          [binds, h = handle, start_stage](rx::script::papyrus::VirtualMachine&) {
            rx::script::papyrus::ObjectRef ref{h};
            binds->StartQuest(ref);
            if (start_stage >= 0) binds->SetStage(ref, start_stage);
          });
      // Open the debugger on the started quest so its stages/objectives show.
      if (edid != "all") quest_panel_.selected = handle;
      ++started;
      if (edid != "all") break;
    }
    RX_INFO("debug: started {} quest(s) matching '{}'", started, edid);
  }

  // The scripted MQ101 playthroughs run host-authoritatively: they drive quest
  // stages and steer NPCs, which a multiplayer client only mirrors via quest /
  // actor replication. Arming them on a client would push stage changes the
  // guest discards in replica mode and steer NPCs the host already owns.
  const bool host = ctx_.config->connect_address.empty();

  // RX_MQ101_DEMO seeds a playable slice of the first main quest: start MQ101
  // (its opening fragment surfaces the first objective), then the npc director
  // drops a waypoint and recruits followers once the player and that objective
  // exist. Walk to the marker to complete the quest.
  if (host && Mq101Demo) {
    const u64 handle = FindQuestHandle("MQ101");
    if (handle != 0) {
      quest_panel_.selected = handle;
      npc_->ArmMq101Demo(handle);
    }
    RX_INFO("debug: MQ101 breadcrumb demo armed (walk the waypoints to complete the quest)");
  }

  // RX_MQ101_SCENE arms an NPC-driven escort: once the player and NPCs exist,
  // a guide NPC leads the player along a path while MQ101 advances to completion.
  if (host && Mq101Scene) {
    const u64 handle = FindQuestHandle("MQ101");
    if (handle != 0) {
      quest_panel_.selected = handle;
      npc_->ArmMq101Scene(handle);
      RX_INFO("debug: MQ101 escort scene armed (a guide NPC will lead the player out)");
    }
  }

  // RX_CW_BATTLE enlists the streamed NPCs around the player into two armies and
  // lets the combat driver fight it out, a live check that the melee path works
  // end to end on real rendered actors. Pairs well with --interior HelgenKeep01.
  if (host && CwBattle) {
    npc_->ArmCwBattle();
    RX_INFO("debug: CW battle harness armed (nearby NPCs split into two armies)");
  }
  // RX_CW_FIELD_BATTLE stages a fresh two-army clash in the open in front of the
  // player, framed for the camera.
  if (host && CwFieldBattle) {
    npc_->ArmCwFieldBattle();
    RX_INFO("debug: CW field battle armed (two lines of soldiers will charge)");
  }
  // RX_CW00_DEMO: the live join beat, walk to General Tullius in Castle Dour
  // and enlist (pair with --interior SolitudeCastleDour RX_PLAYER).
  if (host && Cw00Demo) {
    npc_->ArmCw00Demo();
    RX_INFO("debug: CW00 join demo armed (walk to Tullius to enlist)");
  }
  // RX_CW_DEMO: a playable slice of "Joining the Legion" (CW01A). Start the
  // quest at the clear-the-fort stage (its real fragment surfaces objective 1),
  // stage a fort skirmish the player fights in, and on victory advance to stage
  // 100 (its fragment completes objective 1 and surfaces "Report to Legate
  // Rikke"): combat driving the quest forward.
  if (host && CwDemo) {
    const u64 cw01a = FindQuestHandle("CW01A");
    if (cw01a != 0) {
      quest_panel_.selected = cw01a;
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, cw01a](rx::script::papyrus::VirtualMachine&) {
        binds->StartQuest(rx::script::papyrus::ObjectRef{cw01a});
        binds->SetStage(rx::script::papyrus::ObjectRef{cw01a}, 1);  // "Clear out Fort Hraggstad"
      });
      npc_->ArmCwFieldBattle();
      npc_->set_battle_quest(cw01a, 100);  // victory -> "Report to Legate Rikke"
      RX_INFO("debug: CW01A demo armed (clear the fort, then report to Rikke)");
    }
  }
  // RX_CW_SIEGE_DEMO: start the real fort-siege quest at its
  // battle stage (journal "assist in taking the fort"), stage a two-line clash the
  // player fights in, and on victory advance to its completion stage 9000 ("We
  // have succeeded in taking the fort"): a real Civil War siege quest's journal
  // driven by a battle.
  if (host && CwSiegeDemo) {
    const u64 siege = FindQuestHandle("CWFortSiegeFort");
    if (siege != 0) {
      quest_panel_.selected = siege;
      auto* binds = ctx_.bindings;
      // RX_CW_SIDE enlists the player first (imperial|stormcloak) by advancing
      // that side's intro quest; the C# allegiance tracker reads the side, so the
      // won siege captures the fort for the player's chosen banner. This is the
      // engine-side mirror of the in-game F1/F2 enlistment prompt, both just
      // commit the join quest. Unset leaves the player unaligned (the campaign
      // treats that as the Legion).
      if (const char* side = CwSide.get()) {
        const bool stormcloak = std::string(side) == "stormcloak";
        npc_->set_enlist_quest(FindQuestHandle(stormcloak ? "CW00B" : "CW00A"));
      }
      ctx_.scripts->guest().Submit([binds, siege](rx::script::papyrus::VirtualMachine&) {
        binds->StartQuest(rx::script::papyrus::ObjectRef{siege});
        binds->SetStage(rx::script::papyrus::ObjectRef{siege}, 10);  // "assist in taking the fort"
      });
      npc_->ArmCwFieldBattle();
      npc_->set_battle_quest(siege, 9000);  // victory -> "succeeded in taking the fort"
      // Drive the siege's authored reinforcement pool from the on-screen losses:
      // the HUD bars become the live CWReinforcementPool globals, depleting as
      // soldiers fall through the quest's own ModifyPool (CW = the master quest).
      npc_->set_battle_siege_pool(siege, FindQuestHandle("CW"));
      RX_INFO("debug: CW fort siege demo armed (win the battle to take the fort)");
    }
  }

  // RX_JOURNAL opens the quest journal at load (it is normally toggled with J),
  // for screenshots (cf. RECREATION_UI_MENU / RX_HIDE_DEBUG_UI).
  if (Journal) journal_open_ = true;
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
  // whether the fragment actually dispatched (script loaded + function found),
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
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(id);
    if (stored) ResolveAttachmentForms(records_, stored->winning_plugin, &att);
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
    std::printf("topic [%s] \"%s\" (%zu responses)\n", topic.editor_id.c_str(), topic.text.c_str(),
                topic.responses.size());
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
          .SubmitFor([binds, handle](rx::script::papyrus::VirtualMachine&) {
            using rx::script::papyrus::ObjectRef;
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
              emit(Fmt("  objective %d: %s", o.index,
                       binds->ResolveQuestText(handle, o.text).c_str()));

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
                if (qs.IsObjectiveDisplayed(handle, o.index)) shown += Fmt(" [%d]", o.index);
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
                       o.completed, binds->ResolveQuestText(handle, o.text).c_str()));
            return r;
          })
          .get();
  std::printf("%s", report.c_str());
  std::fflush(stdout);
}

void QuestDirector::ReportQuestList(const std::string& prefix) {
  auto* binds = ctx_.bindings;
  std::string lower_prefix = prefix;
  std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Snapshot the definitions on the guest thread (the only legal QuestSystem
  // caller), sort by editor id, and print on return.
  std::vector<std::pair<u64, std::string>> records(quest_records_.begin(), quest_records_.end());
  std::string report =
      ctx_.scripts->guest()
          .SubmitFor([binds, records = std::move(records),
                      lower_prefix](rx::script::papyrus::VirtualMachine&) mutable {
            quest::QuestSystem& qs = binds->quest_system();
            std::string r;
            auto emit = [&](const std::string& line) {
              r += line;
              r += '\n';
            };

            std::vector<const quest::QuestDef*> defs;
            for (const auto& [h, name] : records) {
              std::string low = name;
              std::transform(low.begin(), low.end(), low.begin(),
                             [](unsigned char c) { return std::tolower(c); });
              if (!lower_prefix.empty() && low.rfind(lower_prefix, 0) != 0) continue;
              if (const quest::QuestDef* def = qs.Definition(h)) defs.push_back(def);
            }
            std::sort(defs.begin(), defs.end(),
                      [](const quest::QuestDef* a, const quest::QuestDef* b) {
                        return a->editor_id < b->editor_id;
                      });
            emit(Fmt("=== %zu quest(s) matching prefix '%s' ===", defs.size(),
                     lower_prefix.c_str()));
            for (const quest::QuestDef* def : defs)
              emit(Fmt("  %-28s 0x%08llx  pri=%-3d stages=%-3zu obj=%-2zu complete@%-4d %s %s",
                       def->editor_id.c_str(), static_cast<unsigned long long>(def->handle),
                       def->priority, def->stages.size(), def->objectives.size(),
                       def->CompletionStage(), def->start_game_enabled ? "SGE" : "   ",
                       def->name.c_str()));
            return r;
          })
          .get();
  std::printf("%s", report.c_str());
  std::fflush(stdout);
}

void QuestDirector::ReportReinforcementTest() {
  const u64 siege = FindQuestHandle("CWFortSiegeFort");
  if (siege == 0 || !ctx_.scripts || !ctx_.bindings) {
    std::printf("reinforcement test: CWFortSiegeFort not found\n");
    return;
  }
  // Resolve the pool globals here (records are immutable; safe off-thread).
  auto glob = [&](const char* edid) -> u64 {
    const bethesda::GlobalFormId g = records_.FindGlobal(edid);
    return g.plugin != 0xffff ? g.packed() : 0;
  };
  const u64 pct_atk = glob("CWPercentPoolRemainingAttacker");
  const u64 pool_atk = glob("CWReinforcementPoolAttacker");

  // Probe a fort Location's LCSR (LocationRefType -> placed-ref) table, the
  // source a find-matching alias resolves against. FortNeugradLocation 0x019183
  // carries the generic-attacker ref-type 0x087a19.
  {
    bethesda::Record loc;
    if (records_.Parse(bethesda::GlobalFormId{0, 0x019183}, &loc)) {
      constexpr u32 kLcsr = FourCc('L', 'C', 'S', 'R');
      for (const bethesda::Subrecord& s : loc.subrecords) {
        if (s.type != kLcsr) continue;
        // Each entry is {LocationRefType: u32, Reference: u32}; stride 8.
        const size_t stride = 8, count = s.data.size() / stride;
        int attackers = 0;
        std::string sample;
        for (size_t i = 0; i < count; ++i) {
          u32 rt = 0, ref = 0;
          std::memcpy(&rt, s.data.data() + i * stride, 4);
          std::memcpy(&ref, s.data.data() + i * stride + 4, 4);
          if (rt == 0x87a19) {  // generic attacker ref-type
            ++attackers;
            if (attackers <= 4) sample += Fmt(" 0x%x", ref);
          }
        }
        std::printf("FortNeugrad LCSR: %zu entries, %d attacker-type(0x87a19) refs e.g.%s\n", count,
                    attackers, sample.c_str());
        std::fflush(stdout);
      }
    }
  }

  auto* binds = ctx_.bindings;
  // The controller writes the pool globals through its CWs cross-reference (the
  // CW master quest, whose script carries the CWReinforcementPoolAttacker
  // GlobalVariable property). In a live game the master quest's setup fills that
  // link; headless we wire it so the propget -> SetValue global write resolves.
  const u64 cw_master = FindQuestHandle("CW");
  std::string report =
      ctx_.scripts->guest()
          .SubmitFor([binds, siege, pct_atk, pool_atk,
                      cw_master](rx::script::papyrus::VirtualMachine& vm) {
            using rx::script::papyrus::ObjectRef;
            std::string r;
            auto emit = [&](const std::string& line) {
              r += line;
              r += '\n';
            };

            // Find an alias that actually carries the reinforcement script (its
            // OnDeath is what drives the pool), scanning the encoded handles.
            i32 alias_id = -1;
            for (i32 id = 0; id < 60; ++id) {
              const ObjectRef ah{rx::script::papyrus::EncodeAliasHandle(siege, id)};
              if (vm.TypeOf(ah).find("Reinforcement") != std::string::npos) {
                alias_id = id;
                break;
              }
            }
            emit(Fmt("=== reinforcement test: CWFortSiegeFort (0x%llx) ===",
                     static_cast<unsigned long long>(siege)));
            emit(Fmt("reinforcement alias id=%d; pool globals: pct=0x%llx pool=0x%llx", alias_id,
                     static_cast<unsigned long long>(pct_atk),
                     static_cast<unsigned long long>(pool_atk)));
            // Verify the new "Find Matching Reference" alias parsing (ALFA/ALRT):
            // these are the slots the siege binds to the fort's placed soldier refs.
            if (const quest::QuestDef* qd = binds->quest_system().Definition(siege)) {
              int fm = 0;
              std::string sample;
              for (const quest::AliasDef& al : qd->aliases) {
                if (!al.find_matching) continue;
                ++fm;
                if (sample.size() < 100)
                  sample += Fmt(" %s(rt=0x%x,parent=%d)", al.name.c_str(), al.ref_type_raw,
                                al.find_in_parent);
              }
              emit(Fmt("find-matching aliases: %d / %zu total;%s", fm, qd->aliases.size(),
                       sample.c_str()));
              // Fill them from a real fort's LCSR (FortNeugradLocation 0x019183)
              // and confirm an attacker slot now resolves to a placed soldier ref.
              const int nfilled =
                  binds->FillFindMatchingAliases(ObjectRef{siege}, ObjectRef{0x019183});
              i32 atk_alias = -1;
              for (const quest::AliasDef& al : qd->aliases)
                if (al.find_matching && al.ref_type_raw == 0x87a19) {
                  atk_alias = al.id;
                  break;
                }
              const u64 atk_ref =
                  atk_alias < 0 ? 0
                                : binds
                                      ->AliasReference(ObjectRef{
                                          rx::script::papyrus::EncodeAliasHandle(siege, atk_alias)})
                                      .handle;
              emit(Fmt("FillFindMatchingAliases: %d filled; attacker alias %d -> ref 0x%llx",
                       nfilled, atk_alias, static_cast<unsigned long long>(atk_ref)));
              // Drive the REAL stage-0 setup: point Alias_Fort at the fort and run
              // Fragment_0. With the find-matching aliases filled, GetLocation +
              // GetReference resolve, so SetUpAliases/RegisterAlias should register
              // the controller's A1.. spawn slots and seed the pool.
              i32 fort_alias = -1;
              for (const quest::AliasDef& al : qd->aliases)
                if (al.name == "Fort") {
                  fort_alias = al.id;
                  break;
                }
              if (fort_alias >= 0)
                binds->ForceAliasLocation(
                    ObjectRef{rx::script::papyrus::EncodeAliasHandle(siege, fort_alias)},
                    ObjectRef{0x019183});
              vm.Call(ObjectRef{siege}, "Fragment_0", {});
              auto memo = [&](const char* v) -> std::string {
                rx::script::papyrus::Value* p = vm.MemberVar(ObjectRef{siege}, v);
                if (!p) return "(absent)";
                if (p->type() == rx::script::papyrus::ValueType::kObject)
                  return Fmt("obj:0x%llx", static_cast<unsigned long long>(p->as_object().handle));
                if (p->type() == rx::script::papyrus::ValueType::kFloat)
                  return Fmt("%.1f", p->as_float());
                return p->is_none() ? "None" : "(other)";
              };
              // A generic Attacker slot (CWReinforcementAliasScript alias) should
              // now hold a placed ref if SetUpAliases ran its ForceRefTo chain.
              u64 gen_ref = 0;
              for (i32 id = 0; id < 60; ++id)
                if (vm.TypeOf(ObjectRef{rx::script::papyrus::EncodeAliasHandle(siege, id)})
                        .find("Reinforcement") != std::string::npos) {
                  gen_ref = binds
                                ->AliasReference(
                                    ObjectRef{rx::script::papyrus::EncodeAliasHandle(siege, id)})
                                .handle;
                  if (gen_ref) break;
                }
              emit(
                  Fmt("after Fragment_0: Fort alias=%d ::PoolAttacker_var=%s; a generic "
                      "reinforcement slot -> ref 0x%llx",
                      fort_alias, memo("::PoolAttacker_var").c_str(),
                      static_cast<unsigned long long>(gen_ref)));

              // Dump the live member names so we can locate the A1..A20
              // registration slots and the attack-point / owner state the
              // self-driving respawn loop keys off of.
              {
                std::string all;
                for (const std::string& n : vm.MemberNames(ObjectRef{siege})) {
                  all += ' ';
                  all += n;
                }
                emit(Fmt("  siege members:%s", all.c_str()));
                if (cw_master) {
                  std::string cm;
                  for (const std::string& n : vm.MemberNames(ObjectRef{cw_master})) {
                    cm += ' ';
                    cm += n;
                  }
                  emit(Fmt("  CW master members:%s", cm.c_str()));
                }
              }
            }

            // Start the siege and run its battle-stage setup (seeds the pools,
            // registers/enables the soldier aliases).
            binds->StartQuest(ObjectRef{siege});
            binds->SetStage(ObjectRef{siege}, 0);
            binds->SetStage(ObjectRef{siege}, 10);
            emit(Fmt("after setup: stage=%d running=%d", binds->GetStage(ObjectRef{siege}),
                     binds->IsRunning(ObjectRef{siege})));

            using rx::script::papyrus::Value;
            auto gv = [&](u64 h) { return h ? binds->GetGlobalValue(ObjectRef{h}) : -1.0f; };
            auto memf = [&](const char* var) -> f32 {
              Value* p = vm.MemberVar(ObjectRef{siege}, var);
              return p ? p->as_float() : -1.0f;
            };

            // Seed the controller's reinforcement pool and wire its CWs
            // cross-reference to the CW master quest (whose script carries the
            // CWReinforcementPoolAttacker GlobalVariable property). In a live siege
            // the stage-0 setup does this off the fort's placed refs, which a
            // headless run has no cell for; seeding it here isolates the
            // death-driven pool Papyrus so we can prove it runs end to end.
            auto seed = [&](const char* var) {
              if (Value* p = vm.MemberVar(ObjectRef{siege}, var)) *p = Value::Float(40.0f);
            };
            seed("::PoolAttacker_var");
            seed("::StartingPoolAttacker_var");
            if (cw_master)
              if (Value* p = vm.MemberVar(ObjectRef{siege}, "::CWs_var"))
                *p = Value::Object(ObjectRef{cw_master});

            const f32 member_before = memf("::PoolAttacker_var");
            emit(Fmt("before: PoolAttacker_var=%.1f global=%.1f CWs=0x%llx", member_before,
                     gv(pool_atk), static_cast<unsigned long long>(cw_master)));

            // Fill a reinforcement-script alias with a soldier and kill it: proves
            // the death dispatches to CWReinforcementAliasScript.OnDeath ->
            // GetOwningQuest().registerDeath, the engine half of the chain (the
            // event trace shows 'OnDeath -> 0x4000... (handler ran)').
            if (alias_id >= 0) {
              const u64 alias_h = rx::script::papyrus::EncodeAliasHandle(siege, alias_id);
              const ObjectRef soldier{(0xFFFFull << 32) | 0x5050};
              binds->SetActorValue(soldier, "health", 80.0f);  // base+current full
              binds->AliasForceRefTo(ObjectRef{alias_h}, soldier);
              binds->ModActorValue(soldier, "health", -200.0f);  // damage to death (base intact)
              const bool dead = binds->IsDead(soldier);
              // The reinforcement controller recycles soldiers via
              // GetActorReference().Reset(): exercise that real native and confirm
              // it brings the dead soldier back (full health, no longer dead).
              vm.Call(ObjectRef{soldier}, "Reset", {});
              emit(Fmt("killed soldier filling alias 0x%llx (%s); dead=%d -> Reset -> dead=%d",
                       static_cast<unsigned long long>(alias_h),
                       vm.TypeOf(ObjectRef{alias_h}).c_str(), dead, binds->IsDead(soldier)));
              binds->ModActorValue(soldier, "health", -200.0f);  // re-down for the pool check below
            }

            // Drive the real pool-write Papyrus: ModifyPool decrements PoolAttacker
            // _var and writes it to CWReinforcementPoolAttacker through the CWs ->
            // property chain. Assert the global updates (a bare global form had no
            // resolvable SetValue before the GlobalVariable dispatch fix, so every
            // script pool-write silently no-opped).
            // ModifyPool(Bool ModAttackerPool, Int NumSoldiersToModifyBy): a
            // sanity check that the pool-write Papyrus updates the global in
            // isolation. -1 soldier => PoolAttacker_var drops by costForReinforcement.
            vm.Call(ObjectRef{siege}, "ModifyPool", {Value::Bool(true), Value::Int(-1)});
            const f32 g = gv(pool_atk), m = memf("::PoolAttacker_var");
            emit(Fmt("after ModifyPool(true,-1): PoolAttacker_var=%.1f global=%.1f", m, g));
            if (g == m && g < member_before) {
              emit(
                  "=> the real reinforcement ModifyPool Papyrus writes "
                  "CWReinforcementPoolAttacker");
              emit("   end to end (controller member -> CWs property -> GlobalVariable.SetValue).");
            } else {
              emit("=> ModifyPool did not write the global (regression!).");
            }

            // Seed the campaign context a headless run lacks (hold ownership and
            // soldier factions), then let the controller's own Papyrus drive a
            // death through the reinforcement chain with no manual ModifyPool:
            // RegisterAlias classifies the attacker into an A-slot, then the death
            // runs tryToRespawnAliass -> TryToRespawnAlias -> IsAliasAttacker ->
            // SubtractFromAttackerPool -> ModifyPool -> global.
            if (cw_master) {
              using rx::script::papyrus::ValueType;
              auto cwm = [&](const char* v) { return vm.MemberVar(ObjectRef{cw_master}, v); };
              auto ctl = [&](const char* v) { return vm.MemberVar(ObjectRef{siege}, v); };
              auto handle = [&](Value* p) -> u64 {
                return (p && p->type() == ValueType::kObject) ? p->as_object().handle : 0;
              };
              auto int_or = [&](Value* p, i32 d) -> i32 {
                return (p && p->type() == ValueType::kInt) ? p->as_int() : d;
              };
              const i32 iImperials = int_or(cwm("::iImperials_var"), 1);
              const i32 iSons = int_or(cwm("::iSons_var"), 2);
              const i32 cost = int_or(ctl("::costForReinforcement_var"), 1);
              const u64 owner_kw = handle(cwm("::CWOwner_var"));
              const u64 sons_fac = handle(cwm("::CWSonsFaction_var"));
              const u64 imp_fac = handle(cwm("::CWImperialFaction_var"));
              emit(
                  Fmt("campaign constants: iImperials=%d iSons=%d cost=%d CWOwner=0x%llx "
                      "ImperialFaction=0x%llx SonsFaction=0x%llx",
                      iImperials, iSons, cost, static_cast<unsigned long long>(owner_kw),
                      static_cast<unsigned long long>(imp_fac),
                      static_cast<unsigned long long>(sons_fac)));

              if (owner_kw && sons_fac) {
                // Stage the battle: the Imperials own (defend) the fort, the
                // Stormcloaks attack. Owner=Imperial => GetAttacker(point)=Sons.
                const ObjectRef point{0x019183};
                vm.Call(ObjectRef{siege}, "RegisterAttackPoint", {Value::Object(point)});
                binds->SetKeywordData(point, ObjectRef{owner_kw}, static_cast<f32>(iImperials));

                // Wire the controller to the CW master and make the pool finite so
                // SubtractFromAttackerPool actually decrements (infinite no-ops).
                if (Value* p = ctl("::CWs_var")) *p = Value::Object(ObjectRef{cw_master});
                if (Value* p = ctl("::InfiniteRespawnAttacker_var")) *p = Value::Bool(false);
                if (Value* p = ctl("::PoolAttacker_var")) *p = Value::Float(40.0f);
                if (Value* p = ctl("::StartingPoolAttacker_var")) *p = Value::Float(40.0f);

                // A Stormcloak trooper fills a reinforcement alias; register it
                // through the REAL classifier so it lands in an attacker A-slot.
                const u64 slot_h = rx::script::papyrus::EncodeAliasHandle(siege, alias_id);
                const ObjectRef trooper{(0xFFFFull << 32) | 0x6060};
                binds->AddToFaction(trooper, ObjectRef{sons_fac});
                binds->SetActorValue(trooper, "health", 100.0f);
                binds->AliasForceRefTo(ObjectRef{slot_h}, trooper);

                // Verify, through the engine API, that every input the classifier
                // reads is correct: the alias resolves to the trooper, the trooper
                // is a Stormcloak (not an Imperial), the fort's owner is Imperial,
                // so the attacking side is the Sons, which is what should make a
                // Stormcloak count as an attacker.
                emit(Fmt(
                    "  classifier inputs: alias->0x%llx in_sons=%d in_imp=%d owner=%.0f "
                    "GetAttacker=%d (want iSons=%d) AttackPoint=0x%llx",
                    static_cast<unsigned long long>(
                        binds->AliasReference(ObjectRef{slot_h}).handle),
                    binds->IsInFaction(trooper, ObjectRef{sons_fac}),
                    binds->IsInFaction(trooper, ObjectRef{imp_fac}),
                    binds->GetKeywordData(point, ObjectRef{owner_kw}),
                    vm.Call(ObjectRef{cw_master}, "GetAttacker", {Value::Object(point)}).as_int(),
                    iSons, static_cast<unsigned long long>(handle(ctl("AttackPoint")))));

                auto find_slot = [&]() -> std::string {
                  for (int i = 1; i <= 20; ++i)
                    if (handle(ctl(Fmt("A%d", i).c_str())) == slot_h) return Fmt("A%d", i);
                  return "(unregistered)";
                };
                // A prior registerDeath may have parked the controller in its
                // "Respawning" state; reset to the base state first.
                vm.GotoState(ObjectRef{siege}, "");
                // The REAL classifier registers the trooper: RegisterAlias reads
                // its faction through GetActorReference() (now a resolvable
                // `... as Actor` cast on the cell-less ref) and compares the side
                // to GetAttacker(AttackPoint).
                vm.Call(ObjectRef{siege}, "RegisterAlias", {Value::Object(ObjectRef{slot_h})});
                const std::string aslot = find_slot();
                emit(Fmt("  registered trooper -> %s", aslot.c_str()));

                const f32 pool_before = ctl("::PoolAttacker_var")->as_float();
                // The trooper dies; drive the controller's real respawn dispatcher.
                binds->ModActorValue(trooper, "health", -200.0f);
                const bool dead = binds->IsDead(trooper);
                vm.Call(ObjectRef{siege}, "tryToRespawnAliass", {});
                const f32 pool_after = ctl("::PoolAttacker_var")->as_float();
                const f32 g2 = gv(pool_atk);
                emit(Fmt("self-driving: trooper in %s, dead=%d; pool %.0f -> %.0f, global=%.0f",
                         aslot.c_str(), dead, pool_before, pool_after, g2));
                if (pool_after == pool_before - static_cast<f32>(cost) && g2 == pool_after) {
                  emit("=> a registered soldier's DEATH self-drove the reinforcement pool down by");
                  emit("   one through the controller's own Papyrus (tryToRespawnAliass ->");
                  emit(
                      "   TryToRespawnAlias -> SubtractFromAttackerPool -> ModifyPool -> global),");
                  emit("   with no manual ModifyPool: the siege loop runs itself.");
                } else {
                  emit("=> respawn dispatcher ran but the pool held (regression: check the staged");
                  emit("   constants and the bare-ref type resolver).");
                }
              }
            }
            return r;
          })
          .get();
  std::printf("%s", report.c_str());
  std::fflush(stdout);
}

void QuestDirector::ReportSceneFragments(const std::string& edid) {
  const u64 handle = FindQuestHandle(edid);
  if (handle == 0 || !ctx_.scripts) {
    std::printf("scene report: no quest matching '%s'\n", edid.c_str());
    return;
  }

  std::vector<SceneJob> jobs = GatherQuestScenes(records_, ctx_.scripts, handle);

  auto* binds = ctx_.bindings;
  std::string report =
      ctx_.scripts->guest()
          .SubmitFor([binds, handle, edid,
                      jobs = std::move(jobs)](rx::script::papyrus::VirtualMachine&) mutable {
            using rx::script::papyrus::ObjectRef;
            quest::QuestSystem& qs = binds->quest_system();
            std::string r;
            auto emit = [&](const std::string& line) {
              r += line;
              r += '\n';
            };

            binds->StartQuest(ObjectRef{handle});
            emit(Fmt("=== scene fragments for %s (0x%llx): %zu scene(s), start stage=%d ===",
                     edid.c_str(), static_cast<unsigned long long>(handle), jobs.size(),
                     qs.GetStage(handle)));
            int advancing = 0;
            for (SceneJob& j : jobs) {
              // Register the scene so GetOwningQuest() resolves and the Run* calls
              // find the fragments, then fire each in phase order, noting the ones
              // that move the stage.
              binds->SetSceneFragments(j.handle, handle, j.frags);
              emit(Fmt("scene %s (0x%llx): begin='%s' end='%s' phases=%zu", j.edid.c_str(),
                       static_cast<unsigned long long>(j.handle), j.frags.begin.function.c_str(),
                       j.frags.end.function.c_str(), j.frags.phases.size()));
              i32 before = qs.GetStage(handle);
              auto note = [&](const std::string& what) {
                const i32 now = qs.GetStage(handle);
                if (now != before) {
                  emit(Fmt("  %s -> stage %d", what.c_str(), now));
                  before = now;
                  ++advancing;
                }
              };
              binds->RunSceneBegin(j.handle);
              note(Fmt("begin %s", j.frags.begin.function.c_str()));
              std::vector<u32> phases;
              for (const auto& p : j.frags.phases) phases.push_back(p.phase);
              std::sort(phases.begin(), phases.end());
              phases.erase(std::unique(phases.begin(), phases.end()), phases.end());
              for (u32 phase : phases)
                for (bool on_begin : {true, false}) {
                  binds->RunScenePhase(j.handle, phase, on_begin);
                  note(Fmt("phase %u %s", phase, on_begin ? "begin" : "end"));
                }
              binds->RunSceneEnd(j.handle);
              note(Fmt("end %s", j.frags.end.function.c_str()));
            }
            emit(Fmt("result: %d fragment(s) advanced the journal, stage=%d complete=%s", advancing,
                     qs.GetStage(handle), qs.IsComplete(handle) ? "YES" : "no"));
            return r;
          })
          .get();
  std::printf("%s", report.c_str());
  std::fflush(stdout);
}

void QuestDirector::AttachQuestScenes(u64 quest) {
  if (!ctx_.scripts || !ctx_.bindings) return;
  std::vector<SceneJob> jobs = GatherQuestScenes(records_, ctx_.scripts, quest);
  if (jobs.empty()) return;
  const size_t n = jobs.size();
  auto* binds = ctx_.bindings;
  ctx_.scripts->guest().Submit(
      [binds, quest, jobs = std::move(jobs)](rx::script::papyrus::VirtualMachine&) mutable {
        for (SceneJob& j : jobs) binds->SetSceneFragments(j.handle, quest, std::move(j.frags));
      });
  RX_INFO("quest: attached {} scene script(s) for 0x{:x}", n, quest);
}

void QuestDirector::ReportSceneLive(const std::string& edid) {
  const u64 handle = FindQuestHandle(edid);
  if (handle == 0 || !ctx_.scripts) {
    std::printf("scene live: no quest matching '%s'\n", edid.c_str());
    return;
  }
  AttachQuestScenes(handle);

  auto* binds = ctx_.bindings;
  // Start the quest, then tick the ScenePlayer: its stage fragments Start scenes,
  // whose fragments SetStage, which run more stage fragments. We report how far
  // it gets on its own (no breadcrumb, no direct stages).
  std::string report =
      ctx_.scripts->guest()
          .SubmitFor([binds, handle, edid](rx::script::papyrus::VirtualMachine&) {
            using rx::script::papyrus::ObjectRef;
            quest::QuestSystem& qs = binds->quest_system();
            std::string r;
            auto emit = [&](const std::string& line) {
              r += line;
              r += '\n';
            };

            binds->StartQuest(ObjectRef{handle});
            const quest::QuestDef* def = qs.Definition(handle);
            emit(Fmt("=== scene live: %s (0x%llx) start stage=%d ===", edid.c_str(),
                     static_cast<unsigned long long>(handle), qs.GetStage(handle)));
            if (!def) {
              emit("no definition");
              return r;
            }

            // Walk the quest's stages in order, the gameplay a player would do
            // (reach a mark, finish a fight) that the engine cannot simulate
            // headless. Setting a stage runs its fragment; where that fragment
            // calls Scene.Start, the scene plays here and its own fragments drive
            // further stages. So this shows the scenes participating natively.
            std::vector<i32> order;
            for (const quest::StageDef& s : def->stages) order.push_back(s.index);
            std::sort(order.begin(), order.end());
            order.erase(std::unique(order.begin(), order.end()), order.end());

            constexpr f32 kDt = 0.5f;
            u32 before_total = binds->scenes_begun();
            for (i32 stage : order) {
              if (qs.GetStageDone(handle, stage)) continue;  // a scene already passed it
              const u32 before = binds->scenes_begun();
              binds->SetStage(ObjectRef{handle}, stage);
              // Let any scene this stage started play to its end.
              for (int t = 0; t < 200 && binds->AnyScenePlaying(); ++t) binds->TickScenes(kDt);
              if (binds->scenes_begun() != before)
                emit(Fmt("  stage %d started %u scene(s) -> stage now %d", stage,
                         binds->scenes_begun() - before, qs.GetStage(handle)));
            }
            emit(Fmt("result: %u scene(s) played, stage=%d complete=%s",
                     binds->scenes_begun() - before_total, qs.GetStage(handle),
                     qs.IsComplete(handle) ? "YES" : "no"));
            return r;
          })
          .get();
  std::printf("%s", report.c_str());
  std::fflush(stdout);
}

void QuestDirector::ReportScenePlay(const std::string& edid) {
  const u64 handle = FindQuestHandle(edid);
  if (handle == 0 || !ctx_.scripts) {
    std::printf("scene play: no quest matching '%s'\n", edid.c_str());
    return;
  }
  std::vector<SceneJob> jobs = GatherQuestScenes(records_, ctx_.scripts, handle);

  auto* binds = ctx_.bindings;
  // Drive each scene through the ScenePlayer over simulated time (the live
  // mechanism, vs ReportSceneFragments which fires every fragment at once), and
  // report which advance the journal. All on the guest thread, the bindings'
  // only legal caller, so the player's cues can call the fragment runners.
  std::string report =
      ctx_.scripts->guest()
          .SubmitFor([binds, handle, edid,
                      jobs = std::move(jobs)](rx::script::papyrus::VirtualMachine&) mutable {
            using rx::script::papyrus::ObjectRef;
            quest::QuestSystem& qs = binds->quest_system();
            std::string r;
            auto emit = [&](const std::string& line) {
              r += line;
              r += '\n';
            };

            binds->StartQuest(ObjectRef{handle});
            emit(Fmt("=== scene play: %s (0x%llx): %zu scene(s), start stage=%d ===", edid.c_str(),
                     static_cast<unsigned long long>(handle), jobs.size(), qs.GetStage(handle)));

            SceneCueSink sink;
            sink.b = binds;
            quest::ScenePlayer player;
            constexpr f32 kDt = 0.5f;            // simulated frame step
            constexpr f32 kPhaseSeconds = 1.5f;  // dwell per phase
            int advancing = 0;
            for (SceneJob& j : jobs) {
              binds->SetSceneFragments(j.handle, handle, j.frags);
              std::vector<u32> phases;
              for (const auto& p : j.frags.phases) phases.push_back(p.phase);
              std::sort(phases.begin(), phases.end());
              phases.erase(std::unique(phases.begin(), phases.end()), phases.end());

              const i32 before = qs.GetStage(handle);
              player.Start(j.handle, phases, kPhaseSeconds, sink);
              for (int guard = 0; player.IsPlaying(j.handle) && guard < 100000; ++guard)
                player.Tick(kDt, sink);
              const i32 now = qs.GetStage(handle);
              emit(Fmt("scene %s (0x%llx): %zu phase(s), stage %d -> %d", j.edid.c_str(),
                       static_cast<unsigned long long>(j.handle), phases.size(), before, now));
              if (now != before) ++advancing;
            }
            emit(Fmt("result: %d scene(s) advanced the journal, stage=%d complete=%s", advancing,
                     qs.GetStage(handle), qs.IsComplete(handle) ? "YES" : "no"));
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
      ctx_.scripts->guest().Submit(
          [binds, handle, objective, displayed](script::papyrus::VirtualMachine&) {
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
      ctx_.scripts->guest().Submit(
          [binds, handle, objective, completed](script::papyrus::VirtualMachine&) {
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
      RX_INFO("quest: placed marker for objective {} of 0x{:x} -> advance to stage {}", objective,
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
              out.panel.push_back({binds->ResolveQuestText(handle, name), handle,
                                   qs.IsRunning(handle), qs.IsActive(handle), qs.IsComplete(handle),
                                   qs.GetStage(handle)});
            }
            out.running = qs.RunningStatuses();
            // Expand <Alias=>/<Global=> tokens in the live HUD text against the
            // filled aliases and global values (guest thread owns both).
            for (quest::QuestStatus& q : out.running) {
              q.name = binds->ResolveQuestText(q.handle, q.name);
              q.log_entry = binds->ResolveQuestText(q.handle, q.log_entry);
              for (quest::ObjectiveStatus& o : q.objectives)
                o.text = binds->ResolveQuestText(q.handle, o.text);
            }
            // Expand the selected quest into stages and objectives for the debugger.
            if (selected != 0) {
              out.detail.handle = selected;
              if (const quest::QuestDef* def = qs.Definition(selected)) {
                out.detail.editor_id = def->editor_id;
                out.detail.completion_stage = def->CompletionStage();
                for (const quest::StageDef& s : def->stages)
                  out.detail.stages.push_back(
                      {s.index, s.log_entry, qs.GetStageDone(selected, s.index)});
              }
              quest::QuestStatus st = qs.Status(selected);
              for (const quest::ObjectiveStatus& o : st.objectives)
                out.detail.objectives.push_back(
                    {o.index, binds->ResolveQuestText(selected, o.text), o.displayed, o.completed});
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
    if (tracked && q->handle == tracked->handle)
      journal_selected = static_cast<int>(journal.size());
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
#if RECREATION_HAS_NET
  if (ctx_.client_session) {
    DriveObjectiveMarkerHud(remote_marker_active_, remote_marker_pos_);
    return;
  }
#endif

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
#if RECREATION_HAS_NET
    if (!reached)
      world_.Each<net::NetworkId, world::Transform>(
          [&](ecs::Entity, net::NetworkId&, world::Transform& t) {
            if (world::MarkerReached(t.position, marker, armed->radius)) reached = true;
          });
#endif
    if (reached) {
      armed->fired = true;
      const u64 quest = armed->quest;
      const i32 stage = armed->advance_stage;
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, quest, stage](script::papyrus::VirtualMachine&) {
        binds->SetStage(script::papyrus::ObjectRef{quest}, stage);
      });
      RX_INFO("quest: reached objective {} marker, advancing 0x{:x} to stage {}", armed->objective,
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

  // Cache the tracked quest's current objective world target (independent of
  // any armed demo marker) so the guided playthrough can head toward the real
  // location rather than a blind facing direction.
  cur_objective_valid_ = false;
  if (hud_tracked_quest_ != 0)
    for (const quest::QuestStatus& q : running) {
      if (q.handle != hud_tracked_quest_) continue;
      for (const quest::ObjectiveStatus& o : q.objectives) {
        if (!o.displayed || o.completed) continue;
        if (ObjectiveTargetFor(q.handle, o.index, &cur_objective_target_)) {
          cur_objective_valid_ = true;
          break;
        }
      }
      break;
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
    const bool changed = active != sent_marker_active_ || quest != sent_marker_quest_ ||
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
  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  if (top.size() > 40) top.resize(40);
  native_trace_panel_.top = std::move(top);
}

}  // namespace rx

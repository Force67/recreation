#include "cutscene_director.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <base/option.h>

#include "actor_system.h"
#include "ai_package_director.h"
#include "bethesda/record.h"
#include "bethesda/strings.h"
#include "bethesda/script_attachment.h"
#include "core/log.h"
#include "dialogue/dialogue.h"
#include "dialogue/voice.h"
#include "engine_internal.h"
#include "interaction_system.h"
#include "npc_director.h"
#include "quest/ctda.h"
#include "quest/scene_record.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "script/papyrus/alias_handle.h"
#include "world/cell_streaming.h"
#include "world/components.h"

namespace rx {
namespace {

// The dialogue camera takes over for a scene the player can see. Off leaves the
// scene playing in the ordinary gameplay view (its lines and packages still run).
base::Option<bool> SceneCamera{"scene.camera", true, "RX_SCENE_CAMERA"};
// Voice playback, and with it the authored pacing. Off falls back to reading time.
base::Option<bool> SceneVoice{"scene.voice", true, "RX_SCENE_VOICE"};
// Letterbox bars while a scene owns the view.
base::Option<bool> SceneLetterbox{"scene.letterbox", true, "RX_SCENE_LETTERBOX"};
// RX_SCENE_SHOT pins every shot to one kind (0 over-shoulder, 1 reverse, 2 two-shot,
// 3 close-up, 4 wide), for framing work and for checking a staged scene from a known
// distance. -1 leaves the shot director in charge.
base::Option<int> SceneShot{"scene.shot", -1, "RX_SCENE_SHOT"};

constexpr u64 kPlayerHandle = 0x14;
constexpr f32 kUnitsToMeters = 0.01428f;
// How near the player has to be for a scene to become their cutscene.
constexpr f32 kWatchRadius = 45.0f;

u32 FormAt(const bethesda::Subrecord* sub, size_t offset = 0) {
  if (!sub || sub->data.size() < offset + 4) return 0;
  u32 raw = 0;
  std::memcpy(&raw, sub->data.data() + offset, 4);
  return raw;
}

}  // namespace

// Bridges a playing scene's cues onto the engine: voice + caption + INFO fragment
// for a line, the AI package driver for a package, the Papyrus phase fragments at
// the phase boundaries (which is what advances the journal).
class CutsceneDirector::Sink : public quest::SceneRuntimeSink {
 public:
  Sink(CutsceneDirector& director, Playing& playing) : d_(director), p_(playing) {}

  void OnSceneBegin(const quest::ScenePlan&) override {
    RX_INFO("cutscene: {} begins ({} phase(s), {} beat(s))", p_.editor_id, p_.plan.phases.size(),
            p_.plan.beats.size());
    d_.RunSceneCue(p_.scene, -1, true, true);
  }

  void OnSceneEnd(const quest::ScenePlan&, bool completed) override {
    RX_INFO("cutscene: {} {}", p_.editor_id, completed ? "done" : "stopped");
    d_.RunSceneCue(p_.scene, -1, false, true);
  }

  void OnPhaseBegin(const quest::ScenePlan&, i32 phase) override {
    d_.RunSceneCue(p_.scene, phase, true, false);
  }

  void OnPhaseEnd(const quest::ScenePlan&, i32 phase) override {
    d_.RunSceneCue(p_.scene, phase, false, false);
  }

  void OnLineBegin(const quest::ScenePlan& plan, const quest::SceneBeat& beat) override {
    p_.speaker = beat.actor;
    p_.addressee = 0;
    // Who the line is aimed at: the head-track target the record names, else the
    // player when they are in the cast, else the previous speaker.
    if (beat.look_at_alias >= 0)
      for (const quest::SceneBeat& other : plan.beats)
        if (other.alias == beat.look_at_alias && other.actor) p_.addressee = other.actor;
    if (p_.addressee == 0 && d_.PlayerInCast(plan)) p_.addressee = kPlayerHandle;
    if (p_.addressee == 0)
      for (const quest::SceneBeat& other : plan.beats)
        if (other.kind == quest::SceneBeat::Kind::kDialogue && other.actor &&
            other.actor != beat.actor)
          p_.addressee = other.actor;

    p_.caption_speaker = beat.speaker;
    p_.caption = beat.text;
    if (!beat.text.empty()) {
      ++d_.lines_spoken_;
      d_.last_line_ = beat.speaker + ": " + beat.text;
      RX_INFO("cutscene: {} says \"{}\"", beat.speaker.empty() ? "?" : beat.speaker, beat.text);
    }

    // Speak it. The clip plays from the speaker's head so it pans with the camera.
    p_.voice = 0;
    p_.voice_hold = 0;
    auto voice = d_.voice_cache_.find(beat.info);
    if (SceneVoice && voice != d_.voice_cache_.end() && !voice->second.path.empty() && d_.ctx_.audio) {
      Vec3 head;
      if (!d_.HeadOf(beat.actor, &head)) head = d_.ctx_.camera ? d_.ctx_.camera->position() : Vec3{};
      p_.voice = d_.ctx_.audio->PlayAt(voice->second.path, head);
      p_.voice_hold = voice->second.seconds;
    }
    // The line's own Papyrus (a TIF_ fragment on the INFO) runs with it: scene
    // dialogue is one of the ways the game advances a quest.
    if (d_.interaction_ && beat.info) d_.interaction_->RunInfoFragment(beat.info, p_.quest);
  }

  void OnLineEnd(const quest::ScenePlan&, const quest::SceneBeat&) override {
    if (p_.voice && d_.ctx_.audio) d_.ctx_.audio->Stop(p_.voice, 0.05f);
    p_.voice = 0;
    p_.caption.clear();
    p_.caption_speaker.clear();
  }

  void OnPackageBegin(const quest::ScenePlan&, const quest::SceneBeat& beat) override {
    if (beat.actor && d_.packages_)
      d_.packages_->RunScenePackage(beat.actor, beat.package, p_.quest, p_.plugin);
  }

  void OnPackageEnd(const quest::ScenePlan&, const quest::SceneBeat& beat) override {
    if (beat.actor && d_.packages_) d_.packages_->StopScenePackage(beat.actor, beat.package);
  }

  bool ConditionsPass(const quest::ConditionList& conditions) override {
    if (!d_.quests_) return true;
    WorldConditionContext ctx(*d_.quests_, [this](u64 handle, Vec3* out) {
      return d_.HeadOf(handle, out);
    });
    return quest::Evaluate(conditions, ctx);
  }

  bool LineStillPlaying(const quest::SceneBeat&) override { return p_.voice_hold > 0.0f; }

 private:
  CutsceneDirector& d_;
  Playing& p_;
};

CutsceneDirector::CutsceneDirector(EngineContext& ctx, ActorSystem* actors, NpcDirector* npc,
                                   AiPackageDirector* packages)
    : ctx_(ctx), actors_(actors), npc_(npc), packages_(packages) {}

CutsceneDirector::~CutsceneDirector() = default;

void CutsceneDirector::IndexScenes() {
  if (!ctx_.records) return;
  ctx_.records->EachOfType(
      FourCc('S', 'C', 'E', 'N'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        bethesda::Record record;
        if (!ctx_.records->Parse(id, &record)) return;
        const quest::SceneDef def = quest::ParseSceneRecord(id.packed(), record, ctx_.records);
        if (def.quest == 0) return;
        SceneEntry entry;
        entry.scene = id.packed();
        entry.quest = def.quest;
        entry.plugin = stored.winning_plugin;
        entry.flags = def.flags;
        entry.editor_id = record.GetString(FourCc('E', 'D', 'I', 'D'));
        scene_index_[entry.scene] = scenes_.size();
        by_quest_[def.quest].push_back(scenes_.size());
        scenes_.push_back(std::move(entry));
      });
  int auto_start = 0;
  for (const SceneEntry& e : scenes_)
    if (e.flags & quest::kSceneBeginOnQuestStart) ++auto_start;
  RX_INFO("cutscene: indexed {} scene(s) over {} quest(s), {} begin with their quest",
          scenes_.size(), by_quest_.size(), auto_start);
  // From here on Scene.Start belongs to this director rather than the headless
  // timer player, so the guest queues the calls for us.
  if (ctx_.bindings) ctx_.bindings->set_live_scene_playback(true);
}

const quest::QuestDef* CutsceneDirector::QuestDefinition(u64 quest) {
  auto it = quest_defs_.find(quest);
  if (it != quest_defs_.end()) return &it->second;
  if (!ctx_.records) return nullptr;
  const bethesda::GlobalFormId id{static_cast<u16>(quest >> 32), static_cast<u32>(quest)};
  bethesda::Record record;
  if (!ctx_.records->Parse(id, &record)) return nullptr;
  quest::QuestDef def = quest::ParseQuestDefinition(quest, record, ctx_.strings);
  auto [pos, ok] = quest_defs_.emplace(quest, std::move(def));
  return &pos->second;
}

u64 CutsceneDirector::LiveRefForBase(u64 base) const {
  if (!ctx_.world || base == 0) return 0;
  u64 found = 0;
  ctx_.world->Each<world::Npc, world::FormLink>(
      [&](ecs::Entity e, world::Npc& npc, world::FormLink& link) {
        if (found != 0 || npc.base.packed() != base) return;
        if (ctx_.world->Has<world::Hidden>(e) || ctx_.world->Has<world::Deleted>(e)) return;
        found = link.form.packed();
      });
  return found;
}

void CutsceneDirector::ResolveLiveCast(const SceneEntry& entry, const quest::SceneDef& def) {
  if (!ctx_.scripts || !ctx_.bindings) return;
  const quest::QuestDef* qdef = QuestDefinition(entry.quest);
  if (!qdef) return;
  std::vector<i32> unresolved;
  std::vector<i32> find_matching;
  for (const quest::SceneActorDef& actor : def.actors) {
    const u64 key = script::papyrus::EncodeAliasHandle(entry.quest, static_cast<u32>(actor.alias));
    if (live_alias_refs_.count(key)) continue;
    const u64 from_records = AliasReference(*qdef, actor.alias, entry.plugin);
    if (from_records != 0 && LiveActor(from_records)) continue;
    // A unique actor can be placed in several cells (an interior court, a set piece
    // up a mountain) and only one of them is in the world. Bind the alias to the
    // instance that actually exists rather than to whichever placement the records
    // scan happened to pick.
    const quest::AliasDef* adef = qdef->FindAlias(actor.alias);
    // A unique actor names one NPC; a created alias names the base the quest makes
    // its reference from. Either way the base is the identity, so bind to whichever
    // instance of it is in the world.
    const u32 base_raw = adef ? (adef->unique_actor_raw ? adef->unique_actor_raw
                                                        : adef->created_base_raw)
                              : 0;
    if (base_raw != 0 && ctx_.records) {
      const u64 base =
          ctx_.records->ResolveFrom(bethesda::RawFormId{base_raw}, entry.plugin).packed();
      if (const u64 live = LiveRefForBase(base)) {
        live_alias_refs_[key] = live;
        continue;
      }
    }
    if (from_records != 0) continue;
    unresolved.push_back(actor.alias);
    if (adef && adef->find_matching) find_matching.push_back(actor.alias);
  }
  // A find-matching alias names a reference type inside a location rather than a
  // reference, so the quest system has to fill it from that location's own table of
  // placed refs. The location is the one the scene plays in.
  const u64 location = !find_matching.empty() ? SceneLocationForm(entry, def) : 0;
  if (unresolved.empty()) return;
  auto* binds = ctx_.bindings;
  const u64 quest = entry.quest;
  std::vector<std::pair<i32, u64>> filled =
      ctx_.scripts->guest()
          .SubmitFor([binds, quest, unresolved, location](script::papyrus::VirtualMachine&) {
            std::vector<std::pair<i32, u64>> out;
            if (location != 0)
              binds->FillFindMatchingAliases(script::papyrus::ObjectRef{quest},
                                             script::papyrus::ObjectRef{location});
            for (i32 alias : unresolved) {
              const u64 handle =
                  script::papyrus::EncodeAliasHandle(quest, static_cast<u32>(alias));
              const u64 ref = binds->AliasReference(script::papyrus::ObjectRef{handle}).handle;
              if (ref != 0) out.push_back({alias, ref});
            }
            return out;
          })
          .get();
  for (const auto& [alias, ref] : filled)
    live_alias_refs_[script::papyrus::EncodeAliasHandle(quest, static_cast<u32>(alias))] = ref;
  if (!filled.empty())
    RX_INFO("cutscene: {} took {} of its cast from the live alias fills", entry.editor_id,
            filled.size());
}

u64 CutsceneDirector::AliasReference(const quest::QuestDef& def, i32 alias, u16 plugin) const {
  const quest::AliasDef* a = def.FindAlias(alias);
  if (!a || !ctx_.records) return 0;
  if (a->forced_ref_raw)
    return ctx_.records->ResolveFrom(bethesda::RawFormId{a->forced_ref_raw}, plugin).packed();
  if (a->unique_actor_raw) {
    const bethesda::GlobalFormId base =
        ctx_.records->ResolveFrom(bethesda::RawFormId{a->unique_actor_raw}, plugin);
    const bethesda::GlobalFormId placed = ctx_.records->PlacedRefForBase(base);
    if (placed.plugin != 0xffff) return placed.packed();
  }
  // The player fills their own alias; the scene needs to know that so it can aim
  // its lines and its camera at them.
  if (a->name == "Player") return kPlayerHandle;
  auto live = live_alias_refs_.find(
      script::papyrus::EncodeAliasHandle(def.handle, static_cast<u32>(alias)));
  return live != live_alias_refs_.end() ? live->second : 0;
}

std::string CutsceneDirector::AliasName(const quest::QuestDef& def, i32 alias) const {
  const quest::AliasDef* a = def.FindAlias(alias);
  return a ? a->name : std::string();
}

std::string CutsceneDirector::SpeakerName(const quest::QuestDef& def, i32 alias, u16 plugin) {
  const u64 ref = AliasReference(def, alias, plugin);
  if (ref != 0 && ref != kPlayerHandle && ctx_.records) {
    // The performer's own name, from the NPC_ behind the placed reference.
    bethesda::Record achr;
    if (ctx_.records->Parse(
            bethesda::GlobalFormId{static_cast<u16>(ref >> 32), static_cast<u32>(ref)}, &achr)) {
      if (const u32 base_raw = FormAt(achr.Find(FourCc('N', 'A', 'M', 'E')))) {
        const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(
            bethesda::GlobalFormId{static_cast<u16>(ref >> 32), static_cast<u32>(ref)});
        bethesda::Record npc;
        if (ctx_.records->Parse(
                ctx_.records->ResolveFrom(bethesda::RawFormId{base_raw},
                                          stored ? stored->winning_plugin : plugin),
                &npc)) {
          // FULL is a string id in a localized plugin and inline text otherwise.
          if (const bethesda::Subrecord* full = npc.Find(FourCc('F', 'U', 'L', 'L'))) {
            if (ctx_.strings && full->data.size() >= 4) {
              u32 id = 0;
              std::memcpy(&id, full->data.data(), 4);
              if (const base::String* text = ctx_.strings->Find(id))
                if (text->size() > 0) return std::string(text->c_str());
            }
            const std::string inline_text = npc.GetString(FourCc('F', 'U', 'L', 'L'));
            if (!inline_text.empty()) return inline_text;
          }
        }
      }
    }
  }
  std::string name = AliasName(def, alias);
  // Alias names read like "MG01MirabelleAlias"; drop the suffix so a caption does
  // not shout the modeller's naming convention at the player.
  constexpr std::string_view kSuffix = "Alias";
  if (name.size() > kSuffix.size() &&
      name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
    name.resize(name.size() - kSuffix.size());
  return name;
}

bool CutsceneDirector::PlayerInCast(const quest::ScenePlan& plan) const {
  for (const quest::SceneBeat& beat : plan.beats)
    if (beat.actor == kPlayerHandle) return true;
  return false;
}

std::string CutsceneDirector::VoiceTypeFor(const SceneEntry& entry, i32 alias, u64 speaker) {
  if (speaker == kPlayerHandle) return {};  // the player has no recorded lines
  // A placed reference names its base NPC, which carries the voice type.
  if (speaker != 0) {
    const bethesda::GlobalFormId ref{static_cast<u16>(speaker >> 32), static_cast<u32>(speaker)};
    bethesda::Record achr;
    if (ctx_.records->Parse(ref, &achr)) {
      if (const u32 base_raw = FormAt(achr.Find(FourCc('N', 'A', 'M', 'E')))) {
        const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(ref);
        const bethesda::GlobalFormId base = ctx_.records->ResolveFrom(
            bethesda::RawFormId{base_raw}, stored ? stored->winning_plugin : entry.plugin);
        const std::string voice = dialogue::VoiceTypeEditorId(*ctx_.records, base);
        if (!voice.empty()) return voice;
      }
    }
  }
  // Most of a scene's cast is not placed anywhere (a unique actor the quest moves
  // in, a created reference); the alias still names the NPC, and the NPC carries
  // the voice type, so the recording is still addressable.
  const quest::QuestDef* qdef = QuestDefinition(entry.quest);
  const quest::AliasDef* adef = qdef ? qdef->FindAlias(alias) : nullptr;
  if (!adef || adef->unique_actor_raw == 0) return {};
  return dialogue::VoiceTypeEditorId(
      *ctx_.records,
      ctx_.records->ResolveFrom(bethesda::RawFormId{adef->unique_actor_raw}, entry.plugin));
}

CutsceneDirector::VoiceLine CutsceneDirector::ResolveVoice(const SceneEntry& entry, i32 alias,
                                                          u64 topic, u64 topic_quest, u64 speaker,
                                                          u64 info, int response_index,
                                                          const std::string& text) {
  VoiceLine line;
  line.seconds = dialogue::EstimateLineSeconds(text);
  if (!ctx_.records || info == 0) return line;
  const std::string voice_type = VoiceTypeFor(entry, alias, speaker);
  if (voice_type.empty()) return line;
  line.had_voice_type = true;

  // The archive index is the reliable route: it keys every recording by the INFO it
  // belongs to, so a line whose clip was exported under some other quest's name
  // still resolves. The name-building path below stays as the fallback.
  if (ctx_.vfs) {
    voices_.Build(*ctx_.vfs);
    const std::string indexed = voices_.Find(static_cast<u32>(info & 0xffffffffu), voice_type);
    if (!indexed.empty()) {
      line.path = indexed;
      if (auto bytes = ctx_.vfs->Read(indexed)) {
        const f32 seconds = dialogue::ClipSeconds(ByteSpan{bytes->data(), bytes->size()});
        if (seconds > 0.1f) line.seconds = seconds;
      }
      return line;
    }
  }

  if (voice_type.empty()) return line;  // nothing to build a name from
  // The file is named after the quest that owns the TOPIC, which is not always the
  // quest that owns the scene: scenes borrow each other's dialogue (a town's shared
  // topics, a questline's greetings), and the recording stays filed under its own.
  std::vector<std::string> quest_names;
  if (topic_quest != 0 && topic_quest != entry.quest)
    if (const quest::QuestDef* tq = QuestDefinition(topic_quest))
      if (!tq->editor_id.empty()) quest_names.push_back(tq->editor_id);
  const quest::QuestDef* qdef = QuestDefinition(entry.quest);
  if (qdef && !qdef->editor_id.empty()) quest_names.push_back(qdef->editor_id);
  if (quest_names.empty()) quest_names.push_back(std::string());
  std::string topic_edid;
  if (topic) {
    bethesda::Record dial;
    if (ctx_.records->Parse(
            bethesda::GlobalFormId{static_cast<u16>(topic >> 32), static_cast<u32>(topic)}, &dial))
      topic_edid = dial.GetString(FourCc('E', 'D', 'I', 'D'));
  }
  // Voice assets are filed under the plugin that authored the line; the master the
  // quest came from is the first guess, then whatever else is loaded that could
  // override it.
  std::vector<std::string> plugins;
  if (const bethesda::PluginFile* p = ctx_.records->PluginAt(
          static_cast<u16>(info >> 32)))
    plugins.push_back(p->file_name());
  if (const bethesda::PluginFile* p = ctx_.records->PluginAt(entry.plugin))
    plugins.push_back(p->file_name());

  std::vector<std::string> candidates;
  for (const std::string& quest_edid : quest_names)
    for (std::string& path :
         dialogue::VoiceFileCandidates(plugins, voice_type, quest_edid, topic_edid,
                                       static_cast<u32>(info & 0xffffffffu), response_index))
      candidates.push_back(std::move(path));
  for (const std::string& path : candidates) {
    if (ctx_.audio && !ctx_.audio->HasAsset(path)) continue;
    line.path = path;
    // Length straight off the clip's header: the recording's own pacing is what the
    // game times a scene phase against, and reading it beats decoding the audio.
    if (ctx_.vfs) {
      if (auto bytes = ctx_.vfs->Read(path)) {
        const f32 seconds = dialogue::ClipSeconds(ByteSpan{bytes->data(), bytes->size()});
        if (seconds > 0.1f) line.seconds = seconds;
      }
    }
    break;
  }
  return line;
}

bool CutsceneDirector::ResolveLine(const SceneEntry& entry, i32 alias, u64 topic, u64 speaker,
                                   u64* info, std::string* text, f32* seconds) {
  if (!ctx_.records || topic == 0) return false;
  const bethesda::GlobalFormId dial{static_cast<u16>(topic >> 32), static_cast<u32>(topic)};
  const dialogue::Topic parsed = dialogue::ParseTopic(*ctx_.records, dial, ctx_.strings);
  if (parsed.responses.empty()) return false;
  // A scene topic holds one INFO per conditioned variant of the beat (race, gender,
  // which side the player took). The engine cannot evaluate all of those, so take
  // the first that carries text, which is the authored default.
  const dialogue::Response* pick = &parsed.responses[0];
  int index = 1;
  for (size_t i = 0; i < parsed.responses.size(); ++i) {
    if (parsed.responses[i].npc_line.empty()) continue;
    pick = &parsed.responses[i];
    index = static_cast<int>(i) + 1;
    break;
  }
  *info = pick->info;
  *text = pick->npc_line;
  auto cached = voice_cache_.find(pick->info);
  if (cached == voice_cache_.end()) {
    // The response index in the file name counts responses on the INFO, not INFOs
    // in the topic; scene lines are single-response, so 1 is right for them.
    VoiceLine voice =
        ResolveVoice(entry, alias, topic, parsed.quest, speaker, pick->info, 1, pick->npc_line);
    cached = voice_cache_.emplace(pick->info, std::move(voice)).first;
  }
  *seconds = cached->second.seconds;
  (void)index;
  return true;
}

quest::ScenePlan CutsceneDirector::BuildPlan(const SceneEntry& entry, const quest::SceneDef& def) {
  const quest::QuestDef* qdef = QuestDefinition(entry.quest);
  quest::ScenePlanBindings bindings;
  if (qdef) {
    const quest::QuestDef& q = *qdef;
    const u16 plugin = entry.plugin;
    bindings.actor = [this, &q, plugin](i32 alias) { return AliasReference(q, alias, plugin); };
    bindings.alias_name = [this, &q, plugin](i32 alias) { return SpeakerName(q, alias, plugin); };
  }
  bindings.line = [this, &entry](i32 alias, u64 topic, u64 speaker, u64* info, std::string* text,
                                 f32* seconds) {
    return ResolveLine(entry, alias, topic, speaker, info, text, seconds);
  };
  bindings.conditions = [this, &entry](const std::vector<quest::SceneRawCondition>& raw) {
    quest::ConditionList out;
    for (const quest::SceneRawCondition& c : raw) {
      quest::Comparison cmp;
      if (quest::ParseCtda(ByteSpan{c.ctda.data(), c.ctda.size()}, &cmp))
        out.comparisons.push_back(cmp);
    }
    if (ctx_.records) quest::ResolveConditionForms(out, *ctx_.records, entry.plugin);
    return out;
  };
  return quest::BuildScenePlan(def, bindings);
}

void CutsceneDirector::EnsureSceneScripts(SceneEntry& entry) {
  if (entry.scripts_attached || !ctx_.scripts || !ctx_.bindings || !ctx_.records) return;
  entry.scripts_attached = true;
  const bethesda::GlobalFormId id{static_cast<u16>(entry.scene >> 32),
                                  static_cast<u32>(entry.scene)};
  bethesda::Record record;
  if (!ctx_.records->Parse(id, &record)) return;
  const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
  if (!vmad) return;
  bethesda::ScriptAttachment attachment;
  bethesda::SceneFragments frags;
  if (!bethesda::ParseSceneFragments(vmad->data, &attachment, &frags) || attachment.scripts.empty())
    return;
  bethesda::ResolveScriptObjectForms(&attachment, [&](u32 raw) {
    return ctx_.records->ResolveFrom(bethesda::RawFormId{raw}, entry.plugin).packed();
  });
  ctx_.scripts->AttachScripts(entry.scene, attachment);
  auto* binds = ctx_.bindings;
  const u64 scene = entry.scene;
  const u64 quest = entry.quest;
  ctx_.scripts->guest().Submit(
      [binds, scene, quest, frags = std::move(frags)](script::papyrus::VirtualMachine&) mutable {
        binds->SetSceneFragments(scene, quest, std::move(frags));
      });
}

void CutsceneDirector::EnableCast(u64 quest, const std::string& editor_id,
                                  const std::vector<u64>& cast) {
  // A scene's performers have to be present: the games place their set pieces
  // initially disabled and switch them on as the quest reaches them, so a scene
  // whose cast is still disabled has outrun that switch. Enabling them goes through
  // the quest world, so it is recorded against the scene's quest and rolls back
  // with it. Retried while the scene plays, because a performer only becomes
  // reachable once its cell has streamed in.
  if (!ctx_.quest_world || !ctx_.world) return;
  std::vector<world::WorldCommand> enable;
  for (u64 ref : cast) {
    const ecs::Entity e = ctx_.quest_world->Find(ref);
    if (!ctx_.world->IsAlive(e) || !ctx_.world->Has<world::Hidden>(e)) continue;
    world::WorldCommand cmd;
    cmd.op = world::WorldOp::kSetEnabled;
    cmd.quest = quest;
    cmd.handle = ref;
    cmd.enabled = true;
    enable.push_back(cmd);
  }
  if (enable.empty()) return;
  ctx_.quest_world->Apply(enable);
  RX_INFO("cutscene: {} switched on {} of its cast", editor_id, enable.size());
}

void CutsceneDirector::GroundCast(const std::vector<u64>& cast) {
  if (!ctx_.streamer || !ctx_.quest_world || !ctx_.world || ctx_.streamer->in_interior()) return;
  for (u64 ref : cast) {
    const ecs::Entity e = ctx_.quest_world->Find(ref);
    if (!ctx_.world->IsAlive(e) || ctx_.world->Has<world::Hidden>(e)) continue;
    world::Transform* t = ctx_.world->Get<world::Transform>(e);
    if (!t) continue;
    f32 ground = 0;
    if (!ctx_.streamer->GroundHeight(t->position[0], t->position[2], &ground)) continue;
    // Only lift, and only a real burial: nudging every actor onto the heightfield
    // would drop the ones standing on a floor, a bridge or a cart.
    const f32 sunk = ground - t->position[1];
    if (sunk > 0.5f && sunk < 8.0f) t->position[1] = ground;
  }
}

void CutsceneDirector::PoseCast(const std::vector<u64>& cast) {
  // The game's standing movement idle: the clip every actor holds when it is doing
  // nothing else, and the one that puts a talking actor on its feet.
  static constexpr const char* kIdleClip = "meshes/actors/character/animations/mt_idle_a_base.hkx";
  if (!ctx_.quest_world || !ctx_.world) return;
  for (u64 ref : cast) {
    if (posed_cast_.count(ref)) continue;
    const ecs::Entity e = ctx_.quest_world->Find(ref);
    if (!ctx_.world->IsAlive(e) || ctx_.world->Has<world::Hidden>(e)) continue;
    if (!actors_->HasNpcInstance(e)) continue;
    if (actors_->PlayNpcClip(e, kIdleClip)) posed_cast_.insert(ref);
  }
}

void CutsceneDirector::RunSceneCue(u64 scene, int phase, bool on_begin, bool scene_edge) {
  if (!ctx_.scripts || !ctx_.bindings) return;
  auto* binds = ctx_.bindings;
  ctx_.scripts->guest().Submit(
      [binds, scene, phase, on_begin, scene_edge](script::papyrus::VirtualMachine&) {
        if (scene_edge) {
          if (on_begin)
            binds->RunSceneBegin(scene);
          else
            binds->RunSceneEnd(scene);
          return;
        }
        binds->RunScenePhase(scene, static_cast<u32>(phase), on_begin);
      });
}

u64 CutsceneDirector::FindQuestByEditorId(const std::string& editor_id) {
  for (const auto& [quest, indices] : by_quest_) {
    const quest::QuestDef* def = QuestDefinition(quest);
    if (def && def->editor_id == editor_id) return quest;
  }
  return 0;
}

u64 CutsceneDirector::SceneLocationForm(const SceneEntry& entry, const quest::SceneDef& def) {
  const quest::QuestDef* qdef = QuestDefinition(entry.quest);
  if (!qdef || !ctx_.records) return 0;
  for (const quest::SceneActorDef& actor : def.actors) {
    const u64 ref = AliasReference(*qdef, actor.alias, entry.plugin);
    if (ref == 0 || ref == kPlayerHandle) continue;
    const bethesda::GlobalFormId id{static_cast<u16>(ref >> 32), static_cast<u32>(ref)};
    const bethesda::GlobalFormId cell = ctx_.records->InteriorCellOfRef(id);
    if (cell.plugin == 0xffff) continue;
    bethesda::Record crec;
    if (!ctx_.records->Parse(cell, &crec)) continue;
    // XLCN: the location the cell belongs to, which is what a find-matching alias
    // searches inside.
    if (const u32 raw = FormAt(crec.Find(FourCc('X', 'L', 'C', 'N')))) {
      const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(cell);
      return ctx_.records
          ->ResolveFrom(bethesda::RawFormId{raw}, stored ? stored->winning_plugin : entry.plugin)
          .packed();
    }
  }
  return 0;
}

std::string CutsceneDirector::SceneLocation(const SceneEntry& entry, const quest::SceneDef& def) {
  const quest::QuestDef* qdef = QuestDefinition(entry.quest);
  if (!qdef || !ctx_.records) return {};
  for (const quest::SceneActorDef& actor : def.actors) {
    const u64 ref = AliasReference(*qdef, actor.alias, entry.plugin);
    if (ref == 0 || ref == kPlayerHandle) continue;
    const bethesda::GlobalFormId id{static_cast<u16>(ref >> 32), static_cast<u32>(ref)};
    const bethesda::GlobalFormId cell = ctx_.records->InteriorCellOfRef(id);
    if (cell.plugin != 0xffff) {
      bethesda::Record crec;
      if (ctx_.records->Parse(cell, &crec)) {
        const std::string edid = crec.GetString(FourCc('E', 'D', 'I', 'D'));
        if (!edid.empty()) return edid;
      }
      continue;
    }
    Vec3 at;
    if (!HeadOf(ref, &at)) continue;
    constexpr f32 kCellSize = 4096.0f;  // game units
    const int cx = static_cast<int>(std::floor(at.x / kUnitsToMeters / kCellSize));
    const int cy = static_cast<int>(std::floor(-at.z / kUnitsToMeters / kCellSize));
    return Fmt("exterior %d,%d", cx, cy);
  }
  return {};
}

bool CutsceneDirector::ArmedSceneLocation(Vec3* pos) {
  if (armed_quest_ == 0 || !ctx_.records) return false;

  auto it = by_quest_.find(armed_quest_);
  if (it == by_quest_.end()) return false;
  const quest::QuestDef* qdef = QuestDefinition(armed_quest_);
  if (!qdef) return false;
  // Prefer the scene that starts with the quest (that is the one about to play),
  // then any of its scenes, and inside it the first performer with a placement.
  std::vector<size_t> order;
  for (size_t index : it->second)
    if (scenes_[index].flags & quest::kSceneBeginOnQuestStart) order.push_back(index);
  for (size_t index : it->second) order.push_back(index);
  for (size_t index : order) {
    const SceneEntry& entry = scenes_[index];
    bethesda::Record record;
    if (!ctx_.records->Parse(
            bethesda::GlobalFormId{static_cast<u16>(entry.scene >> 32),
                                   static_cast<u32>(entry.scene)},
            &record))
      continue;
    const quest::SceneDef def = quest::ParseSceneRecord(entry.scene, record, ctx_.records);
    // Where the scene happens is where its cast stands. Take the medoid of their
    // placements rather than the first one: a cast is spread over everyone's own
    // mark (some of them at the far end of the sequence), and the medoid lands in
    // the group the scene actually opens on.
    std::vector<std::pair<std::string, Vec3>> cast;
    for (const quest::SceneActorDef& actor : def.actors) {
      const u64 ref = AliasReference(*qdef, actor.alias, entry.plugin);
      if (ref == 0 || ref == kPlayerHandle) continue;
      const bethesda::GlobalFormId id{static_cast<u16>(ref >> 32), static_cast<u32>(ref)};
      if (ctx_.records->InteriorCellOfRef(id).plugin != 0xffff) continue;  // needs an exterior
      Vec3 head;
      if (!HeadOf(ref, &head)) continue;
      cast.push_back({AliasName(*qdef, actor.alias), head});
    }
    if (cast.empty()) continue;
    size_t best = 0;
    f32 best_total = 0;
    for (size_t i = 0; i < cast.size(); ++i) {
      f32 total = 0;
      for (size_t j = 0; j < cast.size(); ++j) total += Length(cast[i].second - cast[j].second);
      if (i == 0 || total < best_total) {
        best = i;
        best_total = total;
      }
    }
    for (const auto& [name, at] : cast)
      RX_INFO("cutscene:   cast {} at ({:.0f}, {:.0f})", name, at.x, at.z);
    *pos = cast[best].second;
    RX_INFO("cutscene: {} plays around ({:.0f}, {:.0f}), on {}", entry.editor_id, pos->x, pos->z,
            cast[best].first);
    return true;
  }
  return false;
}

bool CutsceneDirector::StartScene(u64 scene) {
  auto it = scene_index_.find(scene);
  if (it == scene_index_.end()) return false;
  if (IsPlaying(scene)) return true;
  SceneEntry& entry = scenes_[it->second];
  const bethesda::GlobalFormId id{static_cast<u16>(scene >> 32), static_cast<u32>(scene)};
  bethesda::Record record;
  if (!ctx_.records || !ctx_.records->Parse(id, &record)) return false;
  const quest::SceneDef def = quest::ParseSceneRecord(scene, record, ctx_.records);

  EnsureSceneScripts(entry);
  ResolveLiveCast(entry, def);
  auto playing = std::make_unique<Playing>();
  playing->scene = scene;
  playing->quest = entry.quest;
  playing->plugin = entry.plugin;
  playing->editor_id = entry.editor_id;
  playing->plan = BuildPlan(entry, def);
  if (const quest::QuestDef* qdef = QuestDefinition(entry.quest))
    for (const quest::SceneActorDef& actor : def.actors) {
      const u64 ref = AliasReference(*qdef, actor.alias, entry.plugin);
      if (ref != 0 && ref != kPlayerHandle) playing->cast.push_back(ref);
    }
  EnableCast(entry.quest, entry.editor_id, playing->cast);
  Playing& p = *playing;
  Sink sink(*this, p);
  p.runtime.Start(&p.plan, sink);
  if (ctx_.bindings) ctx_.bindings->SetScenePlayingLive(scene, true);
  playing_.push_back(std::move(playing));
  return true;
}

void CutsceneDirector::StopScene(u64 scene) {
  for (size_t i = 0; i < playing_.size(); ++i) {
    if (playing_[i]->scene != scene) continue;
    Sink sink(*this, *playing_[i]);
    playing_[i]->runtime.Stop(sink);
    if (playing_[i]->voice && ctx_.audio) ctx_.audio->Stop(playing_[i]->voice, 0.1f);
    if (ctx_.bindings) ctx_.bindings->SetScenePlayingLive(scene, false);
    playing_.erase(playing_.begin() + static_cast<ptrdiff_t>(i));
    return;
  }
}

bool CutsceneDirector::IsPlaying(u64 scene) const {
  for (const auto& p : playing_)
    if (p->scene == scene) return true;
  return false;
}

void CutsceneDirector::OnQuestStarted(u64 quest) {
  auto it = by_quest_.find(quest);
  if (it == by_quest_.end()) return;
  for (size_t index : it->second) {
    if (!(scenes_[index].flags & quest::kSceneBeginOnQuestStart)) continue;
    RX_INFO("cutscene: {} starts with its quest", scenes_[index].editor_id);
    StartScene(scenes_[index].scene);
  }
}

bool CutsceneDirector::LiveActor(u64 handle) const {
  if (handle == kPlayerHandle) return actors_->HasPlayer();
  if (!ctx_.quest_world || !ctx_.world) return false;
  const ecs::Entity e = ctx_.quest_world->Find(handle);
  return ctx_.world->IsAlive(e) && !ctx_.world->Has<world::Hidden>(e);
}

bool CutsceneDirector::HeadOf(u64 handle, Vec3* out) const {
  if (handle == kPlayerHandle) {
    if (actors_->HasPlayer()) {
      Vec3 feet;
      if (actors_->PlayerWorldPos(&feet)) {
        *out = Vec3{feet.x, feet.y + 1.6f, feet.z};
        return true;
      }
    }
    *out = ctx_.walk_eye;
    return true;
  }
  if (!ctx_.quest_world || !ctx_.world) return false;
  const ecs::Entity e = ctx_.quest_world->Find(handle);
  if (ctx_.world->IsAlive(e)) {
    const world::Transform* t = ctx_.world->Get<world::Transform>(e);
    const Vec3 body = t ? Vec3{t->position[0], t->position[1] + 1.6f, t->position[2]} : Vec3{};
    Vec3 head;
    // The head bone is the better framing point, but only if it is where the body
    // is: a rig whose pose has not settled can report a head metres away, and a
    // camera aimed there frames empty ground.
    if (actors_->NpcHeadWorld(e, &head) && (!t || Length(head - body) < 2.5f)) {
      *out = head;
      return true;
    }
    if (t) {
      *out = body;
      return true;
    }
  }
  // Not streamed in: the authored placement still tells the camera where the scene
  // is happening, which is what a wide shot needs.
  if (!ctx_.records) return false;
  bethesda::Record record;
  if (!ctx_.records->Parse(
          bethesda::GlobalFormId{static_cast<u16>(handle >> 32), static_cast<u32>(handle)},
          &record))
    return false;
  const bethesda::Subrecord* data = record.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 12) return false;
  f32 p[3];
  std::memcpy(p, data->data.data(), 12);
  *out = Vec3{p[0] * kUnitsToMeters, p[2] * kUnitsToMeters + 1.6f, -p[1] * kUnitsToMeters};
  return true;
}

void CutsceneDirector::DriveCamera(f32 dt) {
  // Which scene is the viewer's cutscene: one with a line on screen, whose speaker
  // is really in the world in front of them. Scenes play all over Skyrim at once
  // (the ambient conversations in every town), so a scene the viewer cannot see
  // must never take their camera. A quest armed for watching wins ties.
  Vec3 viewer{};
  if (!actors_->HasPlayer() || !actors_->PlayerWorldPos(&viewer))
    viewer = ctx_.camera ? ctx_.camera->position() : Vec3{};

  const Playing* subject = nullptr;
  Vec3 speaker_head{}, listener_head{};
  f32 best = kWatchRadius;
  for (const auto& p : playing_) {
    // The camera holds on the last speaker between lines: a scene is one continuous
    // shot sequence, not a series of cuts back to the gameplay view.
    if (p->speaker == 0) continue;
    if (!LiveActor(p->speaker)) continue;
    Vec3 head;
    if (!HeadOf(p->speaker, &head)) continue;
    const f32 distance = Length(head - viewer);
    // A run armed to watch one quest follows it wherever it plays: the camera cuts
    // to the scene and the world streams in around it. Every other scene has to be
    // within earshot, so the ambient conversations in every town never hijack the
    // view.
    const bool armed = p->quest == armed_quest_ && armed_quest_ != 0;
    if (distance > kWatchRadius && !armed) continue;
    // An armed quest's scene outranks anything else in earshot, and a scene with a
    // line on screen outranks one between lines.
    if (subject && !armed && distance >= best) continue;
    if (subject && !subject->caption.empty() && p->caption.empty()) continue;
    if (subject && subject->quest == armed_quest_ && armed_quest_ != 0 && !armed) continue;
    subject = p.get();
    speaker_head = head;
    best = distance;
  }
  bool have = subject != nullptr;
  bool establishing = false;
  // Nobody speaking yet (a scene walks its cast to their marks before the first
  // line): a run armed to watch a quest still opens on it, wide, so the staging is
  // on screen from the start.
  if (!have && armed_quest_ != 0) {
    for (const auto& p : playing_) {
      if (p->quest != armed_quest_) continue;
      for (u64 ref : p->cast) {
        if (!LiveActor(ref) || !HeadOf(ref, &speaker_head)) continue;
        // Prefer a performer that is actually being drawn: an ECS entity with no
        // skinned instance would frame an empty spot.
        const ecs::Entity e = ctx_.quest_world->Find(ref);
        if (!actors_->HasNpcInstance(e)) continue;
        int copies = 0;
        Vec3 other{};
        ctx_.world->Each<world::FormLink, world::Transform>(
            [&](ecs::Entity dup, world::FormLink& link, world::Transform& t) {
              if (link.form.packed() != ref) return;
              ++copies;
              if (dup.index != e.index) other = Vec3{t.position[0], t.position[1], t.position[2]};
            });
        RX_INFO(
            "cutscene: establishing on 0x{:x}, {} part(s) at ({:.0f}, {:.0f}, {:.0f}); {} entity "
            "copies, other at ({:.0f}, {:.0f}, {:.0f})",
            ref, actors_->NpcInstanceParts(e), speaker_head.x, speaker_head.y, speaker_head.z,
            copies, other.x, other.y, other.z);
        subject = p.get();
        establishing = true;
        break;
      }
      if (subject) break;
    }
    have = subject != nullptr;
  }
  if (have && (subject->addressee == 0 || !HeadOf(subject->addressee, &listener_head)))
    listener_head = speaker_head;
  const bool want = have && bool(SceneCamera) && !view_released_;
  subject_scene_ = subject ? subject->scene : 0;
  if (!want) {
    owns_view_ = false;
    framing_valid_ = false;
    shots_.Reset();
    return;
  }

  const f32 speaker_yaw = ctx_.cam_yaw;
  const bool cut = shots_.Update(dt, subject->speaker, subject->addressee);
  const world::ShotParams params;
  const f32 sp[3] = {speaker_head.x, speaker_head.y, speaker_head.z};
  const f32 lp[3] = {listener_head.x, listener_head.y, listener_head.z};
  world::ShotKind kind = establishing ? world::ShotKind::kWide : shots_.kind();
  if (const int forced = SceneShot; forced >= 0 && forced <= 4)
    kind = static_cast<world::ShotKind>(forced);
  world::CineFraming want_framing = world::SolveShot(kind, sp, lp, speaker_yaw, params);
  // Keep the lens out of the ground: a shot solved off two heads can end up inside
  // a slope on Skyrim's terrain.
  if (ctx_.streamer) {
    f32 ground = 0;
    if (ctx_.streamer->GroundHeight(want_framing.eye[0], want_framing.eye[2], &ground))
      want_framing.eye[1] = std::max(want_framing.eye[1], ground + 0.6f);
  }
  if (cut || !framing_valid_) {
    framing_ = want_framing;
    framing_valid_ = true;
  } else {
    framing_ = world::EaseFraming(framing_, want_framing, dt, 7.0f);
  }
  if (!owns_view_)
    RX_INFO("cutscene: camera on {} ({} shot {}) at ({:.0f}, {:.0f}, {:.0f})", subject->editor_id,
            establishing ? "establishing" : "coverage", static_cast<int>(kind), speaker_head.x,
            speaker_head.y, speaker_head.z);
  owns_view_ = true;
}

bool CutsceneDirector::CameraOverride(Vec3* eye, Vec3* target) const {
  if (!owns_view_ || !framing_valid_) return false;
  *eye = Vec3{framing_.eye[0], framing_.eye[1], framing_.eye[2]};
  *target = Vec3{framing_.target[0], framing_.target[1], framing_.target[2]};
  return true;
}

void CutsceneDirector::UpdateOverlay(f32 dt) {
  // The caption belongs to the scene on camera; only when nothing is on camera does
  // any other playing scene get to caption itself.
  const Playing* caption = nullptr;
  for (const auto& p : playing_)
    if (!p->caption.empty() && p->scene == subject_scene_) caption = p.get();
  if (!caption && subject_scene_ == 0)
    for (const auto& p : playing_)
      if (!p->caption.empty()) caption = p.get();
  const bool show = caption != nullptr;
  caption_fade_ = std::clamp(caption_fade_ + (show ? dt * 4.0f : -dt * 4.0f), 0.0f, 1.0f);
  overlay_ = TrailerOverlay{};
  overlay_.active = owns_view_ || caption_fade_ > 0.01f;
  if (!overlay_.active) return;
  overlay_.letterbox = (owns_view_ && SceneLetterbox) ? 1.0f : 0.0f;
  if (caption) {
    overlay_.caption_speaker = caption->caption_speaker;
    overlay_.caption = caption->caption;
  }
  overlay_.caption_alpha = caption_fade_;
}

void CutsceneDirector::Tick(f32 dt, const QuestStateCache& quests) {
  quests_ = &quests;
  // A cutscene runs on wall-clock time, but the frame after a cell load can be
  // seconds long; letting that through would fast-forward a whole conversation in
  // one tick. Cap the step at a slow frame instead.
  dt = std::min(dt, 0.25f);
#if RECREATION_HAS_NET
  const bool replica = ctx_.client_session != nullptr;
#else
  const bool replica = false;
#endif

  // Scenes the guest asked for (Scene.Start / Scene.Stop inside a fragment).
  if (ctx_.bindings && !replica) {
    std::vector<script::skyrim::RecordBackedSkyrimBindings::SceneRequest> requests;
    ctx_.bindings->DrainSceneRequests(requests);
    for (const auto& r : requests) {
      if (r.start)
        StartScene(r.scene);
      else
        StopScene(r.scene);
    }
  }

  // Quests that just came online start the scenes flagged to begin with them.
  if (!replica) {
    for (const auto& [handle, entry] : quests.entries()) {
      if (!entry.running) continue;
      if (std::find(quests_seen_running_.begin(), quests_seen_running_.end(), handle) !=
          quests_seen_running_.end())
        continue;
      quests_seen_running_.push_back(handle);
      OnQuestStarted(handle);
    }
  }

  for (size_t i = 0; i < playing_.size();) {
    Playing& p = *playing_[i];
    if (p.voice_hold > 0) p.voice_hold -= dt;
    p.enable_timer -= dt;
    if (p.enable_timer <= 0) {
      p.enable_timer = 1.0f;
      EnableCast(p.quest, p.editor_id, p.cast);
      GroundCast(p.cast);
      PoseCast(p.cast);
    }
    Sink sink(*this, p);
    p.runtime.Tick(dt, sink);
    if (!p.runtime.playing()) {
      if (p.voice && ctx_.audio) ctx_.audio->Stop(p.voice, 0.1f);
      if (ctx_.bindings) ctx_.bindings->SetScenePlayingLive(p.scene, false);
      playing_.erase(playing_.begin() + static_cast<ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
  if (playing_.empty()) view_released_ = false;

  DriveCamera(dt);
  UpdateOverlay(dt);
  // The armed quest's journal is the thing a cutscene is supposed to move, so log
  // each step: this is what a verification run reads.
  if (armed_quest_ != 0) {
    const i32 stage = quests.Stage(armed_quest_);
    if (stage != armed_stage_) {
      RX_INFO("cutscene: {} at stage {}{}", armed_quest_, stage,
              quests.Complete(armed_quest_) ? " (complete)" : "");
      armed_stage_ = stage;
    }
  }
  quests_ = nullptr;
}

void CutsceneDirector::ReportQuestCutscenes(const std::string& prefix) {
  std::string want = prefix;
  std::transform(want.begin(), want.end(), want.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (want == "all") want.clear();  // every quest that owns a scene

  // Group the indexed scenes by quest, keeping the quests in editor-id order so a
  // questline reads top to bottom.
  std::vector<std::pair<std::string, u64>> quests;
  for (const auto& [quest, indices] : by_quest_) {
    const quest::QuestDef* def = QuestDefinition(quest);
    if (!def) continue;
    std::string edid = def->editor_id;
    std::string low = edid;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!want.empty() && low.rfind(want, 0) != 0) continue;
    quests.push_back({std::move(edid), quest});
  }
  std::sort(quests.begin(), quests.end());

  std::printf("=== cutscenes for %zu quest(s) matching '%s' ===\n", quests.size(),
              prefix.c_str());
  int total_scenes = 0, total_lines = 0, total_voiced = 0;
  // Why a spoken line ends up unvoiced: the speaker has no voice type (an alias
  // nothing fills, a creature, the player) or the named clip is not in the archives.
  int no_voice_type = 0, no_clip = 0;
  for (const auto& [edid, quest] : quests) {
    const quest::QuestDef* def = QuestDefinition(quest);
    int q_scenes = 0, q_lines = 0, q_voiced = 0, q_packages = 0;
    f32 q_seconds = 0;
    std::string detail;
    for (size_t index : by_quest_[quest]) {
      SceneEntry& entry = scenes_[index];
      bethesda::Record record;
      if (!ctx_.records->Parse(bethesda::GlobalFormId{static_cast<u16>(entry.scene >> 32),
                                                      static_cast<u32>(entry.scene)},
                               &record))
        continue;
      const quest::SceneDef sdef = quest::ParseSceneRecord(entry.scene, record, ctx_.records);
      const quest::ScenePlan plan = BuildPlan(entry, sdef);
      int lines = 0, voiced = 0, packages = 0;
      f32 seconds = 0;
      for (const quest::SceneBeat& beat : plan.beats) {
        if (beat.kind == quest::SceneBeat::Kind::kPackage) {
          ++packages;
          continue;
        }
        if (beat.kind != quest::SceneBeat::Kind::kDialogue) continue;
        if (!beat.text.empty()) ++lines;
        auto voice = voice_cache_.find(beat.info);
        if (voice != voice_cache_.end()) {
          if (!voice->second.path.empty())
            ++voiced;
          else if (!beat.text.empty())
            voice->second.had_voice_type ? ++no_clip : ++no_voice_type;
        }
        seconds += beat.seconds;
      }
      ++q_scenes;
      q_lines += lines;
      q_voiced += voiced;
      q_packages += packages;
      q_seconds += seconds;
      // Per-cast detail when the report is aimed at a questline rather than the
      // whole game: which alias fill rule each performer came from, and what it
      // resolved to. This is what says whether a scene has a cast to point at.
      if (!want.empty())
        for (const quest::SceneActorDef& actor : sdef.actors) {
          const quest::AliasDef* adef = def ? def->FindAlias(actor.alias) : nullptr;
          detail += Fmt("      cast %-24s alias %3d forced=%06x unique=%06x match=%d -> %llx\n",
                        adef ? adef->name.c_str() : "?", actor.alias,
                        adef ? adef->forced_ref_raw : 0, adef ? adef->unique_actor_raw : 0,
                        adef ? adef->find_matching : 0,
                        static_cast<unsigned long long>(
                            def ? AliasReference(*def, actor.alias, entry.plugin) : 0));
        }
      detail +=
          Fmt("    %-34s %2zu phases %3zu beats %3d lines %3d voiced %2d packages %5.0fs  %-28s%s\n",
              entry.editor_id.c_str(), plan.phases.size(), plan.beats.size(), lines, voiced,
              packages, seconds, SceneLocation(entry, sdef).c_str(),
              (entry.flags & quest::kSceneBeginOnQuestStart) ? " [starts with quest]" : "");
    }
    std::printf("  %-28s %s: %d scene(s), %d line(s), %d voiced, %d package beat(s), %.0fs\n",
                edid.c_str(), def && !def->name.empty() ? def->name.c_str() : "", q_scenes, q_lines,
                q_voiced, q_packages, q_seconds);
    std::printf("%s", detail.c_str());
    total_scenes += q_scenes;
    total_lines += q_lines;
    total_voiced += q_voiced;
  }
  std::printf(
      "=== %d scene(s), %d spoken line(s), %d with a voice clip; %d unvoiced for want of a "
      "voice type, %d for want of the file ===\n",
      total_scenes, total_lines, total_voiced, no_voice_type, no_clip);
  std::fflush(stdout);
}

std::vector<std::string> CutsceneDirector::Report() const {
  std::vector<std::string> out;
  for (const auto& p : playing_) {
    char line[256];
    const quest::SceneBeat* beat = p->runtime.speaking();
    std::snprintf(line, sizeof(line), "%s phase %d%s%s", p->editor_id.c_str(),
                  p->runtime.phase(), beat ? "  " : "",
                  beat ? (beat->speaker + ": " + beat->text).substr(0, 90).c_str() : "");
    out.push_back(line);
  }
  return out;
}

}  // namespace rx

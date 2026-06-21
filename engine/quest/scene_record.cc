#include "quest/scene_record.h"

#include <cstring>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rec::quest {
namespace {

constexpr u32 kFnam = FourCc('F', 'N', 'A', 'M');
constexpr u32 kPnam = FourCc('P', 'N', 'A', 'M');
constexpr u32 kHnam = FourCc('H', 'N', 'A', 'M');
constexpr u32 kNam0 = FourCc('N', 'A', 'M', '0');
constexpr u32 kNext = FourCc('N', 'E', 'X', 'T');
constexpr u32 kCtda = FourCc('C', 'T', 'D', 'A');
constexpr u32 kWnam = FourCc('W', 'N', 'A', 'M');
constexpr u32 kAlid = FourCc('A', 'L', 'I', 'D');
constexpr u32 kLnam = FourCc('L', 'N', 'A', 'M');
constexpr u32 kDnam = FourCc('D', 'N', 'A', 'M');
constexpr u32 kAnam = FourCc('A', 'N', 'A', 'M');
constexpr u32 kInam = FourCc('I', 'N', 'A', 'M');
constexpr u32 kSnam = FourCc('S', 'N', 'A', 'M');
constexpr u32 kEnam = FourCc('E', 'N', 'A', 'M');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kHtid = FourCc('H', 'T', 'I', 'D');
constexpr u32 kDmax = FourCc('D', 'M', 'A', 'X');
constexpr u32 kDmin = FourCc('D', 'M', 'I', 'N');
constexpr u32 kDemo = FourCc('D', 'E', 'M', 'O');
constexpr u32 kDeva = FourCc('D', 'E', 'V', 'A');

template <typename T>
T ReadLe(const bethesda::Subrecord& sub) {
  T value{};
  if (sub.data.size() >= sizeof(T)) std::memcpy(&value, sub.data.data(), sizeof(T));
  return value;
}

}  // namespace

SceneDef ParseSceneRecord(u64 handle, const bethesda::Record& record,
                          const bethesda::RecordStore* records) {
  SceneDef def;
  def.handle = handle;

  // SCEN form ids (owning quest, dialogue topics, packages) are plugin
  // relative; resolve them against the masters of the plugin that won this
  // record. The handle is the packed GlobalFormId, so it gives us the id to
  // look the winning plugin up.
  u16 plugin = 0;
  bool can_resolve = false;
  if (records) {
    bethesda::GlobalFormId id{static_cast<u16>(handle >> 32), static_cast<u32>(handle)};
    if (const auto* stored = records->Find(id)) {
      plugin = stored->winning_plugin;
      can_resolve = true;
    }
  }
  auto resolve = [&](u32 raw) -> u64 {
    if (raw == 0) return 0;
    if (!can_resolve) return raw;
    return records->ResolveFrom(bethesda::RawFormId{raw}, plugin).packed();
  };

  // Subrecords arrive in declaration order: scene header, then a phase run,
  // then an actor run, then an action run. A small state machine tracks which
  // section we are in. Phases and actions are opened by HNAM/ANAM and closed by
  // their next marker, so each builds into a pending slot flushed on close.
  enum class Section { kHeader, kPhases, kActors, kActions };
  Section section = Section::kHeader;

  ScenePhaseDef phase;
  bool phase_open = false;
  bool phase_in_completion = false;  // a NEXT moved us from begin to completion CTDAs
  i32 phase_counter = 0;

  SceneActionDef action;
  bool action_open = false;
  bool action_end_seen = false;  // ENAM passed: a later SNAM is the timer value

  auto flush_phase = [&] {
    if (phase_open) {
      phase.index = phase_counter++;
      def.phases.push_back(std::move(phase));
      phase = ScenePhaseDef{};
    }
    phase_open = false;
    phase_in_completion = false;
  };
  auto flush_action = [&] {
    if (action_open) def.actions.push_back(std::move(action));
    action = SceneActionDef{};
    action_open = false;
    action_end_seen = false;
  };

  for (const bethesda::Subrecord& sub : record.subrecords) {
    switch (sub.type) {
      case kFnam:
        // Action-level FNAM is the action flags; the scene-level FNAM is the
        // first one, before any action opens.
        if (action_open)
          action.flags = ReadLe<u32>(sub);
        else
          def.flags = ReadLe<u32>(sub);
        break;
      case kPnam:
        // Action-level PNAM is the package ref of a package action; the
        // scene-level PNAM is the owning quest, which trails the action run.
        if (action_open) {
          action.package = resolve(ReadLe<u32>(sub));
        } else {
          def.quest = resolve(ReadLe<u32>(sub));
        }
        break;

      case kHnam:
        // Each phase is bracketed by a pair of HNAMs: the first opens it, the
        // second closes it. The actor section starts at the first ALID after
        // the final closing HNAM.
        section = Section::kPhases;
        if (phase_open) {
          flush_phase();
        } else {
          phase_open = true;
        }
        break;
      case kNam0:
        // Phase name flag while phasing; action name flag inside an action.
        break;
      case kNext:
        if (section == Section::kPhases) phase_in_completion = true;
        break;
      case kCtda:
        if (section == Section::kPhases && phase_open) {
          SceneRawCondition cond;
          cond.ctda.assign(sub.data.begin(), sub.data.end());
          (phase_in_completion ? phase.completion : phase.begin).push_back(std::move(cond));
        }
        break;
      case kWnam:
        break;

      case kAlid:
        // ALID after the phase run opens the actor section; once actions begin,
        // ALID is the action's performing alias.
        if (section == Section::kPhases) {
          flush_phase();
          section = Section::kActors;
        }
        if (section == Section::kActions && action_open) {
          action.actor_alias = ReadLe<i32>(sub);
        } else {
          SceneActorDef actor;
          actor.alias = ReadLe<i32>(sub);
          def.actors.push_back(std::move(actor));
        }
        break;
      case kLnam:
        if (section == Section::kActors && !def.actors.empty())
          def.actors.back().flags = ReadLe<u32>(sub);
        break;
      case kDnam:
        if (section == Section::kActors && !def.actors.empty())
          def.actors.back().flags2 = ReadLe<u32>(sub);
        break;

      case kAnam: {
        section = Section::kActions;
        // A non-empty ANAM opens an action (its type); an empty ANAM closes it.
        if (sub.data.size() >= 2) {
          flush_action();
          action_open = true;
          action.raw_type = ReadLe<u16>(sub);
          switch (action.raw_type) {
            case 0:
              action.kind = SceneActionDef::Kind::kDialogue;
              break;
            case 1:
              action.kind = SceneActionDef::Kind::kPackage;
              break;
            case 2:
              action.kind = SceneActionDef::Kind::kTimer;
              break;
            default:
              action.kind = SceneActionDef::Kind::kUnknown;
              break;
          }
        } else {
          flush_action();
        }
        break;
      }
      case kSnam:
        // The first SNAM is the start phase; a timer action carries a second
        // SNAM after ENAM that is the wait duration in seconds (a float).
        if (action_open) {
          if (action_end_seen)
            action.timer_seconds = ReadLe<f32>(sub);
          else
            action.start_phase = ReadLe<i32>(sub);
        }
        break;
      case kEnam:
        if (action_open) {
          action.end_phase = ReadLe<i32>(sub);
          action_end_seen = true;
        }
        break;
      case kData:
        // Dialogue action: DATA is the DIAL topic to play.
        if (action_open && action.kind == SceneActionDef::Kind::kDialogue)
          action.topic = resolve(ReadLe<u32>(sub));
        break;
      case kHtid:
        if (action_open) action.head_track_alias = ReadLe<i32>(sub);
        break;
      case kDmax:
        if (action_open) action.delay_max = ReadLe<f32>(sub);
        break;
      case kDmin:
        if (action_open) action.delay_min = ReadLe<f32>(sub);
        break;
      case kDemo:
        if (action_open) action.emotion_type = ReadLe<u32>(sub);
        break;
      case kDeva:
        if (action_open) action.emotion_value = ReadLe<i32>(sub);
        break;
      case kInam:
        // Action index / scene action counter; positional, not needed here.
        break;
      default:
        break;
    }
  }

  // A final phase or action may close at end of record without a trailing
  // marker.
  flush_phase();
  flush_action();
  return def;
}

}  // namespace rec::quest

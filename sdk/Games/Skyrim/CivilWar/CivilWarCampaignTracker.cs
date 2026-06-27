using System;
using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Drives the campaign board off the questline and surfaces each capture. It watches
// QuestStageChanged for a Civil War siege/mission quest reaching its won stage and
// hands the matching hold to the side that took it, then toasts the fall and tracks
// that side's war progress on a HUD gauge. Ownership only ever moves through quest
// completion, never a timer: it is a readout over the story, not a simulation.
public sealed class CivilWarCampaignTracker : GameBehaviour
{
    // One hold capture the war can produce: the quest that, on completing, awards
    // `Hold` to `Captor`. CompleteStage is the stage that marks the battle won.
    private readonly struct HoldMission
    {
        public readonly Hold Hold;
        public readonly Side Captor;
        public readonly int CompleteStage;

        public HoldMission(Hold hold, Side captor, int completeStage)
        {
            Hold = hold;
            Captor = captor;
            CompleteStage = completeStage;
        }
    }

    // The stage a Civil War mission/siege quest sets when its battle is won. Vanilla
    // CW mission quests resolve around here; a single shared value keeps the table
    // terse and is the one knob to tune if a probed quest differs.
    private const int MissionWonStage = 200;

    // PLACEHOLDER form ids, in their own range so they are obviously distinct from
    // the real records and from CivilWarForms' ids. Each is a per-side hold-battle
    // quest: the Reunification (Imperial) and Liberation (Stormcloak) campaigns each
    // capture the enemy's holds, both fight the pivotal Battle for Whiterun, and
    // each ends by storming the other capital. Replace with the dumped CWMission*/
    // CWSiege* records once probed; the mapping is the only thing to fix.
    private const uint CWMissionImperialWhiterun = 0x00A10001;   // Battle for Whiterun (Imperial)
    private const uint CWMissionImperialThePale = 0x00A10002;    // Dawnstar
    private const uint CWMissionImperialWinterhold = 0x00A10003; // Winterhold
    private const uint CWMissionImperialTheRift = 0x00A10004;    // Riften
    private const uint CWSiegeImperialWindhelm = 0x00A10005;     // Windhelm (final)

    private const uint CWMissionStormcloakWhiterun = 0x00A10011;   // Battle for Whiterun (Stormcloak)
    private const uint CWMissionStormcloakHjaalmarch = 0x00A10012; // Morthal
    private const uint CWMissionStormcloakFalkreath = 0x00A10013;  // Falkreath
    private const uint CWMissionStormcloakTheReach = 0x00A10014;   // Markarth
    private const uint CWSiegeStormcloakSolitude = 0x00A10015;     // Solitude (final)

    // Quest id -> the capture it completes. Built once; lookup is the membership
    // test the tracker runs on every stage change.
    private static readonly Dictionary<uint, HoldMission> Missions = new()
    {
        [CWMissionImperialWhiterun] = new(Hold.Whiterun, Side.Imperial, MissionWonStage),
        [CWMissionImperialThePale] = new(Hold.ThePale, Side.Imperial, MissionWonStage),
        [CWMissionImperialWinterhold] = new(Hold.Winterhold, Side.Imperial, MissionWonStage),
        [CWMissionImperialTheRift] = new(Hold.TheRift, Side.Imperial, MissionWonStage),
        [CWSiegeImperialWindhelm] = new(Hold.Eastmarch, Side.Imperial, MissionWonStage),

        [CWMissionStormcloakWhiterun] = new(Hold.Whiterun, Side.Stormcloak, MissionWonStage),
        [CWMissionStormcloakHjaalmarch] = new(Hold.Hjaalmarch, Side.Stormcloak, MissionWonStage),
        [CWMissionStormcloakFalkreath] = new(Hold.Falkreath, Side.Stormcloak, MissionWonStage),
        [CWMissionStormcloakTheReach] = new(Hold.TheReach, Side.Stormcloak, MissionWonStage),
        [CWSiegeStormcloakSolitude] = new(Hold.Haafingar, Side.Stormcloak, MissionWonStage),
    };

    // The persistent war-progress bar shown for the side that is advancing.
    private const string WarProgressGauge = "cw_war_progress";
    private const uint ImperialColor = 0xd4af37ffu;    // Legion gold
    private const uint StormcloakColor = 0x3c6ec8ffu;  // Stormcloak blue

    private EventBus.Subscription? _stageSub;

    protected override void OnStart()
    {
        _stageSub = EventBus.Subscribe<QuestStageChanged>(OnStageChanged);
        CivilWarCampaign.HoldCaptured += OnHoldCaptured;
    }

    protected override void OnDestroy()
    {
        _stageSub?.Dispose();
        CivilWarCampaign.HoldCaptured -= OnHoldCaptured;
        Hud.ClearGauge(WarProgressGauge);
        CivilWarCampaign.Reset();
    }

    private void OnStageChanged(QuestStageChanged e)
    {
        // An unresolved handle reports form id 0, which is never in the table, so
        // the lookup is the null-safe gate; no player or live quest is needed.
        if (!Missions.TryGetValue(e.Quest.FormId, out HoldMission mission)) return;
        if (e.Stage < mission.CompleteStage) return;
        CivilWarCampaign.CaptureHold(mission.Hold, mission.Captor);  // idempotent
    }

    // Reacts to a real ownership change: name the fall and update the advancing
    // side's war-progress bar.
    private void OnHoldCaptured(Hold hold, Side side)
    {
        Debug.Notification($"{CivilWarCampaign.CityOf(hold)} has fallen to the {Banner(side)}!");
        Hud.Gauge(WarProgressGauge, CivilWarCampaign.WarProgress(side),
                  $"War Progress ({SideLabel(side)})", GaugeColor(side));
    }

    // How a side is named in the "X has fallen to the Y!" toast.
    private static string Banner(Side side) => side switch
    {
        Side.Imperial => "Legion",
        Side.Stormcloak => "Stormcloaks",
        _ => "no one",
    };

    private static string SideLabel(Side side) => side switch
    {
        Side.Imperial => "Imperial",
        Side.Stormcloak => "Stormcloak",
        _ => "Neutral",
    };

    private static uint GaugeColor(Side side) => side switch
    {
        Side.Imperial => ImperialColor,
        Side.Stormcloak => StormcloakColor,
        _ => 0u,  // let the HUD pick
    };
}

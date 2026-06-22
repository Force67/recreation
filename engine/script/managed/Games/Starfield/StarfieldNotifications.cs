using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Surfaces the Starfield gameplay systems to the player as corner notifications,
// the way Skyrim's QuestProgressTracker and MeditationPower do. It owns no logic:
// it subscribes to the events the survival and progression systems publish and
// turns each into a short status line, the "ui prompt" layer over the simulation.
// Returning null from a mapping skips the prompt (e.g. a non-critical stage).
public sealed class StarfieldNotifications : GameBehaviour
{
    private readonly List<EventBus.Subscription> _subs = new();

    protected override void OnStart()
    {
        On<OxygenStageChanged>(e => e.Stage switch
        {
            OxygenStage.Low => "Oxygen low",
            OxygenStage.Empty => "Out of oxygen",
            _ => null,
        });
        On<CarbonDioxideStageChanged>(e => e.Stage switch
        {
            CarbonDioxideStage.Building => "CO2 building",
            CarbonDioxideStage.Maxed => "CO2 critical",
            _ => null,
        });
        On<AfflictionContracted>(e => $"Afflicted: {e.Id}");
        On<HazardStageChanged>(e => e.Stage == HazardStage.Critical ? $"{e.Hazard} hazard critical" : null);
        On<OverMassChanged>(e => e.OverMass ? "Over-encumbered" : "Mass nominal");
        On<CharacterLeveledUp>(e => $"Level {e.Level}");
        On<SkillRankUp>(e => $"{e.Skill} rank {e.Rank}");
        On<LocationDiscovered>(_ => "Location discovered");
        On<ResearchCompleted>(e => $"Research complete: {e.ProjectId}");
        On<ItemCrafted>(e => "Item crafted");
    }

    protected override void OnDestroy()
    {
        foreach (EventBus.Subscription sub in _subs) sub.Dispose();
        _subs.Clear();
    }

    // Subscribes a handler mapping an event to an optional notification line.
    private void On<T>(Func<T, string?> line) where T : IGameEvent =>
        _subs.Add(EventBus.Subscribe<T>(e =>
        {
            if (line(e) is { } message) Debug.Notification(message);
        }));
}

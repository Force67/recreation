using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Surfaces the Fallout 4 gameplay systems to the player as corner notifications, the
// Fallout twin of StarfieldNotifications. It owns no logic: it subscribes to the
// events the survival and progression systems publish and turns each into a short
// status line, the "ui prompt" layer over the simulation. Returning null from a
// mapping skips the prompt (e.g. a non-critical stage).
public sealed class CommonwealthNotifications : GameBehaviour
{
    private readonly List<EventBus.Subscription> _subs = new();

    protected override void OnStart()
    {
        On<RadiationStageChanged>(e => e.Stage switch
        {
            RadiationStage.Minor => "Radiation: minor sickness",
            RadiationStage.Advanced => "Radiation: advanced sickness",
            RadiationStage.Critical => "Radiation: critical",
            _ => null,  // back to Clean: quiet
        });
        On<CharacterLeveledUp>(e => $"Level {e.Level}");
        On<EncumbranceChanged>(e => e.OverEncumbered ? "Over-encumbered" : "Weight nominal");
        On<FactionStandingChanged>(e => $"{e.Faction}: {e.Standing}");
        On<ChemAddicted>(e => $"Addicted: {e.ChemId}");
        On<AddictionCured>(e => $"Cured: {e.ChemId}");
        On<AdrenalineChanged>(e => e.Rank > 0 ? $"Adrenaline rank {e.Rank}" : "Adrenaline spent");
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

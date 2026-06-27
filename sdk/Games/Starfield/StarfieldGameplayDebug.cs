using System.Collections.Generic;
using System.Linq;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// A keypad debug panel for the Starfield gameplay systems, the gameplay analog of
// the quest-stage debugger: it lets a developer exercise the survival and
// progression loops by hand instead of waiting for play to drive them. Off by
// default; enable with "debug": true in Starfield.json. Each action routes through
// the same systems and events as real play, so the corner notifications confirm it
// worked.
//   Num1  toggle exertion (drain or restore oxygen)
//   Num2  toggle a radiation hazard field
//   Num3  grant character experience
//   Num4  clear afflictions
//   J     rest (restore the breath meters and start the rested bonus)
public sealed class StarfieldGameplayDebug : GameBehaviour
{
    // Character XP granted per Num3 press.
    public float XpPerGrant { get; set; } = 100f;

    private readonly List<EventBus.Subscription> _binds = new();

    protected override void OnStart()
    {
        _binds.Add(Hotkeys.Bind(Key.Num1, ToggleExertion));
        _binds.Add(Hotkeys.Bind(Key.Num2, ToggleHazard));
        _binds.Add(Hotkeys.Bind(Key.Num3, GrantXp));
        _binds.Add(Hotkeys.Bind(Key.Num4, ClearAfflictions));
        _binds.Add(Hotkeys.Bind(Key.J, Rest));
    }

    protected override void OnDestroy()
    {
        foreach (EventBus.Subscription bind in _binds) bind.Dispose();
        _binds.Clear();
    }

    // The live instance of a sibling system, or null when it is not installed.
    private static T? Find<T>() where T : GameBehaviour => ModHost.ActiveBehaviours.OfType<T>().FirstOrDefault();

    private void ToggleExertion()
    {
        if (Find<OxygenCo2>() is not { } oxygen) return;
        oxygen.Exerting = !oxygen.Exerting;
        Debug.Notification(oxygen.Exerting ? "Debug: exerting" : "Debug: resting");
    }

    private void ToggleHazard()
    {
        if (Find<EnvironmentalHazards>() is not { } hazards) return;
        hazards.Hazard = hazards.Hazard == SpaceHazard.None ? SpaceHazard.Radiation : SpaceHazard.None;
        Debug.Notification($"Debug: hazard {hazards.Hazard}");
    }

    private void GrantXp()
    {
        CharacterProgress.GainXp(Game.Player, XpPerGrant);
        Debug.Notification($"Debug: +{XpPerGrant:0} XP (level {CharacterProgress.Level(Game.Player)})");
    }

    private void ClearAfflictions()
    {
        int cured = Afflictions.Cure(Game.Player);
        Debug.Notification($"Debug: cleared {cured} affliction(s)");
    }

    private void Rest()
    {
        if (Find<WellRested>() is not { } rested) return;
        rested.Rest();
        Debug.Notification("Debug: rested");
    }
}

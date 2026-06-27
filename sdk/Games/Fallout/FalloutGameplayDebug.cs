using System.Collections.Generic;
using System.Linq;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// A keypad debug panel for the Fallout 4 gameplay systems, the Fallout twin of
// StarfieldGameplayDebug: it lets a developer exercise the survival and progression
// loops by hand instead of waiting for play to drive them. Off by default; enable
// with "debug": true in Fallout.json. Each action routes through the same systems
// and events as real play, so the corner notifications confirm it worked.
//   Num1  toggle a radiation field (rads climb or stop)
//   Num2  grant character experience
//   Num3  spend VATS action points
//   Num4  clean up: decontaminate and cure all chem addictions (Addictol)
//   J     raise Minutemen reputation
public sealed class FalloutGameplayDebug : GameBehaviour
{
    // Character XP granted per Num2 press.
    public float XpPerGrant { get; set; } = 100f;
    // Action points spent per Num3 press (a VATS shot's worth).
    public float ApPerShot { get; set; } = 25f;
    // Minutemen reputation granted per J press.
    public int ReputationPerHelp { get; set; } = 20;

    private readonly List<EventBus.Subscription> _binds = new();

    protected override void OnStart()
    {
        _binds.Add(Hotkeys.Bind(Key.Num1, ToggleRadiation));
        _binds.Add(Hotkeys.Bind(Key.Num2, GrantXp));
        _binds.Add(Hotkeys.Bind(Key.Num3, SpendActionPoints));
        _binds.Add(Hotkeys.Bind(Key.Num4, CleanUp));
        _binds.Add(Hotkeys.Bind(Key.J, HelpMinutemen));
    }

    protected override void OnDestroy()
    {
        foreach (EventBus.Subscription bind in _binds) bind.Dispose();
        _binds.Clear();
    }

    // The live instance of a sibling system, or null when it is not installed.
    private static T? Find<T>() where T : GameBehaviour => ModHost.ActiveBehaviours.OfType<T>().FirstOrDefault();

    private void ToggleRadiation()
    {
        if (Find<Radiation>() is not { } radiation) return;
        radiation.Irradiated = !radiation.Irradiated;
        Debug.Notification(radiation.Irradiated ? "Debug: in rad field" : "Debug: out of rad field");
    }

    private void GrantXp()
    {
        CharacterProgress.GainXp(Game.Player, XpPerGrant);
        Debug.Notification($"Debug: +{XpPerGrant:0} XP (level {CharacterProgress.Level(Game.Player)})");
    }

    private void SpendActionPoints()
    {
        if (Find<ActionPoints>() is not { } ap) return;
        bool spent = ap.Spend(ApPerShot);
        Debug.Notification(spent ? $"Debug: VATS shot ({ap.Current:0} AP left)" : "Debug: not enough AP");
    }

    private void CleanUp()
    {
        Find<Radiation>()?.Decontaminate();
        int cured = ChemAddiction.CureAll(Game.Player);
        Debug.Notification($"Debug: decontaminated, cured {cured} addiction(s)");
    }

    private void HelpMinutemen()
    {
        FactionReputation.Gain(CommonwealthFaction.Minutemen, ReputationPerHelp);
        Debug.Notification($"Debug: Minutemen +{ReputationPerHelp} " +
                           $"({FactionReputation.StandingOf(CommonwealthFaction.Minutemen)})");
    }
}

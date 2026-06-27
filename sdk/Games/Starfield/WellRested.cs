using System.Linq;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised when the rested bonus turns on or off.
public readonly struct RestedChanged(bool rested) : IGameEvent
{
    public bool Rested { get; } = rested;
}

// Sleep recovery and the Well Rested bonus, the survival loop's recovery action:
// resting tops the breath meters back up and grants a temporary experience boost,
// the way both Starfield and Skyrim reward sleep. A rest sets CharacterProgress's
// XP multiplier for a span of game-time, which OnUpdate winds back on its own.
// Triggered by Rest() from a bed/sleep interaction or the debug panel.
public sealed class WellRested : GameBehaviour
{
    // Fraction added to XP gains while rested (0.1 == +10%).
    public float Bonus { get; set; } = 0.1f;
    // How long the bonus lasts, in game-hours.
    public float DurationGameHours { get; set; } = 24f;

    public bool Rested { get; private set; }

    private float _expiresAtGameTime;

    // Restores the survival meters and starts (or refreshes) the rested bonus.
    public void Rest()
    {
        foreach (OxygenCo2 oxygen in ModHost.ActiveBehaviours.OfType<OxygenCo2>()) oxygen.Restore();
        _expiresAtGameTime = GameClock.GameTime + DurationGameHours / 24f;
        CharacterProgress.XpMultiplier = 1f + Bonus;
        if (Rested) return;
        Rested = true;
        EventBus.Publish(new RestedChanged(true));
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (!Rested || GameClock.GameTime < _expiresAtGameTime) return;
        Rested = false;
        CharacterProgress.XpMultiplier = 1f;
        EventBus.Publish(new RestedChanged(false));
    }

    protected override void OnDestroy()
    {
        if (Rested) CharacterProgress.XpMultiplier = 1f;
    }
}

using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the VATS action-point pool: it starts full, a spend draws it down and is
// refused when too low, regeneration pauses briefly after a spend then refills to
// the max, and Agility raises the maximum through the SPECIAL registry.
public static class FalloutActionPointsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Special.Clear();
        Effects.Clear();
        EventBus.Clear();

        int changes = 0;
        using var sub = EventBus.Subscribe<ActionPointsChanged>(_ => changes++);

        var ap = new ActionPoints
        {
            RegenPerSecond = 20f,
            RegenDelay = 1.0f,
            BaseMax = 60f,
            MaxPerAgility = 10f,
        };
        ModHost.Register(ap);  // OnStart fills the pool to Max
        check.Equal("starts full at base max", 60f, ap.Current);

        // A spend the pool can cover draws it down.
        check.That("can spend within the pool", ap.CanSpend(25f));
        check.That("spend succeeds", ap.Spend(25f));
        check.Equal("pool drawn down", 35f, ap.Current);

        // Regeneration is paused for RegenDelay right after a spend.
        ModHost.Tick(0.5f);  // inside the pause
        check.Equal("regen paused after a spend", 35f, ap.Current);

        // After the delay, regen resumes.
        ModHost.Tick(0.5f);  // pause elapses on this tick (no refill yet)
        ModHost.Tick(1.0f);  // +20
        check.Equal("regenerates after the pause", 55f, ap.Current);

        // It tops out at the max and does not overshoot.
        ModHost.Tick(2.0f);
        check.Equal("refills to the max", 60f, ap.Current);

        // A spend larger than the pool is refused and leaves it untouched.
        check.That("cannot overspend", !ap.Spend(100f));
        check.Equal("pool untouched by a refused spend", 60f, ap.Current);

        // Agility raises the maximum through the SPECIAL registry.
        Special.Set(Game.Player, SpecialAttribute.Agility, 5);  // +4 over rank 1 => +40
        check.Equal("agility raises the max", 100f, ap.Max);
        ap.Refill();
        check.Equal("refill tops to the new max", 100f, ap.Current);

        check.That("pool changes were announced", changes > 0);

        ModHost.Shutdown();
        Special.Clear();
        Effects.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}

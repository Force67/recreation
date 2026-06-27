using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the SPECIAL attribute registry: ranks clamp to 1..10, Strength drives
// CarryWeight and Endurance drives max Health through real actor-value bonuses,
// Intelligence raises the XP rate, the derived bonuses re-size when a rank changes
// rather than stacking, and Clear reverts everything.
public static class FalloutSpecialTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        Special.Clear();
        CharacterProgress.Clear();
        Effects.Clear();
        EventBus.Clear();

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.CarryWeight, 0, 0);
        fake.SetValue(player.Handle, ActorValue.Health, 0, 0);

        int changes = 0;
        using var sub = EventBus.Subscribe<SpecialChanged>(_ => changes++);

        // Every attribute defaults to the Fallout starting rank of 1.
        check.Equal("strength defaults to 1", 1, Special.Rank(player, SpecialAttribute.Strength));

        // Strength 5: +10 CarryWeight per point => +50 over the default rank-1 bonus.
        Special.Set(player, SpecialAttribute.Strength, 5);
        check.Equal("strength clamps in range", 5, Special.Rank(player, SpecialAttribute.Strength));
        check.Equal("strength adds carry weight", 50f, fake.GetCurrent(player.Handle, ActorValue.CarryWeight));
        check.Equal("carry-weight formula", 250f, Special.CarryWeightFor(5));

        // Raising strength resizes the bonus rather than stacking it.
        Special.Set(player, SpecialAttribute.Strength, 8);
        check.Equal("carry weight resized, not stacked", 80f,
                    fake.GetCurrent(player.Handle, ActorValue.CarryWeight));

        // Endurance 6: +5 Health per point => +30.
        Special.Set(player, SpecialAttribute.Endurance, 6);
        check.Equal("endurance adds health", 30f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // Intelligence raises the XP multiplier: +3% per point over 1.
        Special.Set(player, SpecialAttribute.Intelligence, 11);  // clamps to 10
        check.Equal("intelligence clamps to 10", 10, Special.Rank(player, SpecialAttribute.Intelligence));
        check.That("intelligence raises the xp rate", CharacterProgress.XpMultiplier > 1.2f);

        // Increase nudges a rank up one and stops at the cap.
        check.That("increase raises a rank", Special.Increase(player, SpecialAttribute.Luck));
        check.Equal("luck is now 2", 2, Special.Rank(player, SpecialAttribute.Luck));
        Special.Set(player, SpecialAttribute.Luck, 10);
        check.That("increase fails at the cap", !Special.Increase(player, SpecialAttribute.Luck));

        check.That("rank changes were announced", changes > 0);

        // Clear reverts the derived bonuses and the XP multiplier.
        Special.Clear();
        check.Equal("carry weight reverted on clear", 0f, fake.GetCurrent(player.Handle, ActorValue.CarryWeight));
        check.Equal("health reverted on clear", 0f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.Equal("xp multiplier reset on clear", 1f, CharacterProgress.XpMultiplier);
        check.Equal("ranks reset on clear", 1, Special.Rank(player, SpecialAttribute.Strength));

        Special.Clear();
        CharacterProgress.Clear();
        Effects.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}

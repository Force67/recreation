using Recreation.Modding;

namespace Recreation.Tests;

// Covers named cooldown timing and reset.
public static class CooldownsTests
{
    public static void Run(Check check)
    {
        Cooldowns.Clear();

        check.That("unknown key is ready", Cooldowns.IsReady("shout"));

        Cooldowns.Start("shout", 3f);
        check.That("not ready right after start", !Cooldowns.IsReady("shout"));
        check.Equal("full time remaining", 3f, Cooldowns.Remaining("shout"));

        Cooldowns.Advance(1f);
        check.Equal("time counts down", 2f, Cooldowns.Remaining("shout"));
        check.That("still on cooldown", !Cooldowns.IsReady("shout"));

        Cooldowns.Advance(2f);
        check.That("ready once elapsed", Cooldowns.IsReady("shout"));
        check.Equal("no time remaining", 0f, Cooldowns.Remaining("shout"));

        // Keys are independent.
        Cooldowns.Start("power", 5f);
        Cooldowns.Start("shout", 1f);
        check.That("power still cooling", !Cooldowns.IsReady("power"));
        Cooldowns.Advance(1f);
        check.That("shout ready again", Cooldowns.IsReady("shout"));
        check.That("power still cooling after short tick", !Cooldowns.IsReady("power"));

        // Reset makes a key ready immediately.
        Cooldowns.Reset("power");
        check.That("reset clears the cooldown", Cooldowns.IsReady("power"));

        Cooldowns.Clear();
    }
}

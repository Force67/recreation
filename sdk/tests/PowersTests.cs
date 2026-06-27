using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers greater powers: a power fires once, then is on cooldown until a game day
// passes, and the spell type reads back as a power.
public static class PowersTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Powers.Clear();
        Powers.CooldownDays = 1f;

        const ulong battleCry = 0x700, fortifyHealth = 0x800;
        fake.SetSpellInfo(battleCry, cost: 0, type: 2, castType: 1, delivery: 0);  // 2 = power
        fake.SetMagicEffectInfo(fortifyHealth, "Health", detrimental: false);
        fake.SetIngredientEffects(battleCry, (fortifyHealth, 50, 60));  // a 60s +50 health buff

        Spell power = Spell.From(battleCry);
        check.Equal("spell reads back as a power", SpellType.Power, power.Type);

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.Health, current: 100, baseValue: 200);
        fake.GameTime = 5.0f;  // day 5

        // First use fires and applies the effect.
        check.That("power is ready", Powers.Ready(power));
        check.That("using the power fires it", Powers.Use(player, power));
        check.Equal("the power's effect applied", 150f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // It is now on cooldown for the rest of the day.
        check.That("power is on cooldown", !Powers.Ready(power));
        check.That("using it again does nothing", !Powers.Use(player, power));
        check.Equal("no second application", 150f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // Part of a day later it is still on cooldown.
        fake.GameTime = 5.5f;
        check.That("still on cooldown half a day later", !Powers.Ready(power));
        check.That("about half a day remains", Powers.Cooldown(power) > 0.4f && Powers.Cooldown(power) < 0.6f);

        // A full game day later it is ready again.
        fake.GameTime = 6.0f;
        check.That("ready a day later", Powers.Ready(power));
        check.Equal("no cooldown remaining", 0f, Powers.Cooldown(power));
        check.That("it fires again", Powers.Use(player, power));

        Powers.Clear();
        ModHost.Shutdown();
        Native.Backend = null;
    }
}

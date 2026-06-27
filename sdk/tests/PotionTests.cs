using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers consumables: a potion reads its effects, and drinking it restores or
// damages a value instantly, fortifies it for a duration, and skips effects on
// values the engine does not model.
public static class PotionTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        Actor player = Game.Player;
        const ulong restoreHealth = 0xE1, fortifyStamina = 0xE2, damageHealth = 0xE3, exotic = 0xE4;
        fake.SetMagicEffectInfo(restoreHealth, "Health", detrimental: false);
        fake.SetMagicEffectInfo(fortifyStamina, "Stamina", detrimental: false);
        fake.SetMagicEffectInfo(damageHealth, "Health", detrimental: true);
        fake.SetMagicEffectInfo(exotic, "", detrimental: false);  // cannot be mapped to a value

        // A healing potion: one instant Restore Health (magnitude 50, duration 0).
        const ulong healPotion = 0x300;
        fake.SetIngredientEffects(healPotion, (restoreHealth, 50, 0));
        Potion potion = Potion.From(healPotion);
        check.Equal("potion exposes its effects", 1, potion.Effects.Count);
        check.Equal("effect magnitude is read", 50f, potion.Effects[0].Magnitude);

        // Drinking it restores health, capped at base.
        fake.SetValue(player.Handle, ActorValue.Health, current: 30, baseValue: 100);
        check.Equal("one effect applied", 1, potion.Consume(player));
        check.Equal("health restored", 80f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // A poison: instant Damage Health (magnitude 25).
        const ulong poison = 0x301;
        fake.SetIngredientEffects(poison, (damageHealth, 25, 0));
        Potion.From(poison).Consume(player);
        check.Equal("health damaged", 55f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // A fortify potion: timed +40 Stamina for 10 seconds, reverting after.
        const ulong staminaPotion = 0x302;
        fake.SetIngredientEffects(staminaPotion, (fortifyStamina, 40, 10));
        fake.SetValue(player.Handle, ActorValue.Stamina, current: 100, baseValue: 100);
        Potion.From(staminaPotion).Consume(player);
        check.Equal("stamina fortified now", 140f, fake.GetCurrent(player.Handle, ActorValue.Stamina));
        ModHost.Tick(10f);
        check.Equal("fortify reverts after its duration", 100f,
                    fake.GetCurrent(player.Handle, ActorValue.Stamina));

        // An effect on an unmodelled value is skipped.
        const ulong exoticPotion = 0x303;
        fake.SetIngredientEffects(exoticPotion, (exotic, 10, 0));
        check.Equal("unmappable effect is skipped", 0, Potion.From(exoticPotion).Consume(player));

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}

using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers shrines: praying cures disease and grants a timed blessing that fortifies
// a value, holds through its duration, then lapses on its own and reverts.
public static class ShrinesTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        ModHost.Shutdown();
        Blessings.Clear();
        Diseases.Clear();
        EventBus.Clear();

        fake.GameTime = 10.0f;  // day 10

        // Akatosh's blessing fortifies Restoration; Rockjoint weakens melee.
        const ulong akatosh = 0xD00, fortifyRestoration = 0xD10;
        fake.SetMagicEffectInfo(fortifyRestoration, ActorValue.Restoration, detrimental: false);
        fake.SetIngredientEffects(akatosh, (fortifyRestoration, 10, 0));
        const ulong rockjoint = 0xD01, weakenMelee = 0xD11;
        fake.SetMagicEffectInfo(weakenMelee, ActorValue.OneHanded, detrimental: true);
        fake.SetIngredientEffects(rockjoint, (weakenMelee, 20, 0));
        fake.SetSpellInfo(rockjoint, cost: 0, type: (int)SpellType.Disease);

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.Restoration, current: 25, baseValue: 25);
        fake.SetValue(player.Handle, ActorValue.OneHanded, current: 30, baseValue: 30);

        Diseases.Contract(player, Spell.From(rockjoint));
        check.Equal("the disease saps melee", 10f, fake.GetCurrent(player.Handle, ActorValue.OneHanded));

        int expired = 0;
        using var sub = EventBus.Subscribe<BlessingExpired>(_ => expired++);

        // Praying cures the disease and grants the blessing for the default 8 hours.
        Prayer prayer = Shrine.Pray(player, Spell.From(akatosh));
        check.Equal("praying cured the disease", 1, prayer.DiseasesCured);
        check.Equal("melee restored by the cure", 30f, fake.GetCurrent(player.Handle, ActorValue.OneHanded));
        check.Equal("the blessing fortifies restoration", 35f,
                    fake.GetCurrent(player.Handle, ActorValue.Restoration));
        check.Equal("the blessing is current", akatosh, Blessings.Current(player));

        // Four hours in it still holds.
        fake.GameTime = 10.0f + 4f / 24f;
        Blessings.Refresh();
        check.Equal("still blessed mid-duration", akatosh, Blessings.Current(player));
        check.Equal("restoration still fortified", 35f, fake.GetCurrent(player.Handle, ActorValue.Restoration));

        // Past eight hours it lapses on its own and reverts.
        fake.GameTime = 10.0f + 9f / 24f;
        Blessings.Refresh();
        check.Equal("the blessing expired", 0UL, Blessings.Current(player));
        check.Equal("restoration reverted on expiry", 25f,
                    fake.GetCurrent(player.Handle, ActorValue.Restoration));
        check.Equal("an expiry event fired once", 1, expired);

        ModHost.Shutdown();
        Blessings.Clear();
        Diseases.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}

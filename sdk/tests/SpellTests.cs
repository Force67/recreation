using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers spells: reading cost/cast-type/delivery and effects, casting (the magicka
// check, the deduction, applying effects to a target, and self-cast), and the
// player's spellbook.
public static class SpellTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong firebolt = 0x500, damageHealth = 0x601;
        fake.SetSpellInfo(firebolt, cost: 30, castType: 1, delivery: 2);
        fake.SetMagicEffectInfo(damageHealth, "Health", detrimental: true);
        fake.SetIngredientEffects(firebolt, (damageHealth, 40, 0));  // a 40-damage spell

        Spell spell = Spell.From(firebolt);
        check.Equal("spell cost is read", 30, spell.Cost);
        check.Equal("cast type is read", SpellCastType.FireAndForget, spell.CastType);
        check.Equal("delivery is read", SpellDelivery.Aimed, spell.Delivery);
        check.Equal("spell exposes its effects", 1, spell.Effects.Count);

        Actor caster = Game.Player;
        Actor target = Actor.From(0x100);
        fake.SetValue(caster.Handle, ActorValue.Magicka, current: 100, baseValue: 100);
        fake.SetValue(target.Handle, ActorValue.Health, current: 100, baseValue: 100);

        // A successful cast spends the magicka and applies the effect.
        check.That("caster can afford it", spell.CanCast(caster));
        check.That("cast succeeds", spell.Cast(caster, target));
        check.Equal("magicka spent", 70f, fake.GetCurrent(caster.Handle, ActorValue.Magicka));
        check.Equal("target took the effect", 60f, fake.GetCurrent(target.Handle, ActorValue.Health));

        // Too little magicka: nothing happens.
        fake.SetValue(caster.Handle, ActorValue.Magicka, current: 10, baseValue: 100);
        check.That("cannot afford it now", !spell.CanCast(caster));
        check.That("cast fails", !spell.Cast(caster, target));
        check.Equal("magicka untouched", 10f, fake.GetCurrent(caster.Handle, ActorValue.Magicka));
        check.Equal("target untouched", 60f, fake.GetCurrent(target.Handle, ActorValue.Health));

        // A self-cast heal applies to the caster.
        const ulong healSelf = 0x501, restoreHealth = 0x602;
        fake.SetSpellInfo(healSelf, cost: 20, castType: 1, delivery: 0);
        fake.SetMagicEffectInfo(restoreHealth, "Health", detrimental: false);
        fake.SetIngredientEffects(healSelf, (restoreHealth, 50, 0));
        fake.SetValue(caster.Handle, ActorValue.Magicka, current: 100, baseValue: 100);
        fake.SetValue(caster.Handle, ActorValue.Health, current: 30, baseValue: 100);
        check.That("self-cast succeeds", Spell.From(healSelf).Cast(caster));
        check.Equal("self-cast healed the caster", 80f, fake.GetCurrent(caster.Handle, ActorValue.Health));

        // The spellbook teaches and tracks.
        Spellbook.Clear();
        check.Equal("spellbook starts empty", 0, Spellbook.Count);
        check.That("learning teaches the spell", Spellbook.Learn(spell));
        check.That("the player now knows it", fake.HasSpell(fake.Player, firebolt));
        check.That("the spellbook knows it", Spellbook.Knows(spell));
        check.Equal("one spell known", 1, Spellbook.Count);
        check.That("relearning is a no-op", !Spellbook.Learn(spell));

        Spellbook.Clear();
        Native.Backend = null;
    }
}

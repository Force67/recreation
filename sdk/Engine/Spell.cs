using System.Collections.Generic;

namespace Recreation;

// What kind of spell this is (SPIT type).
public enum SpellType
{
    Spell = 0,
    Disease = 1,
    Power = 2,
    LesserPower = 3,
    Ability = 4,
    Poison = 5,
    Voice = 11,
}

// How a spell is cast (SPIT cast type).
public enum SpellCastType
{
    ConstantEffect = 0,
    FireAndForget = 1,
    Concentration = 2,
}

// How a spell reaches its target (SPIT delivery).
public enum SpellDelivery
{
    Self = 0,
    Touch = 1,
    Aimed = 2,
    TargetActor = 3,
    TargetLocation = 4,
}

// A spell (SPEL record): a magicka cost and a set of magic effects. Reading those
// is the engine's job; the gameplay of casting -- the magicka check, the
// deduction and applying the effects -- is soft logic here, so a mod can build a
// working spell with no engine casting system. Projectiles and aiming are the
// engine's domain; the result a cast produces is this.
public sealed class Spell : Form
{
    private Spell(ulong handle) : base(handle) { }

    public static new Spell From(ulong handle) => new(handle);
    public static Spell From(Form form) => new(form.Handle);

    // The magicka the spell costs to cast.
    public int Cost => Call("GetSpellCost").AsInt();
    public SpellType Type => (SpellType)Call("GetSpellType").AsInt();
    public SpellCastType CastType => (SpellCastType)Call("GetSpellCastType").AsInt();
    public SpellDelivery Delivery => (SpellDelivery)Call("GetSpellDelivery").AsInt();

    // The magic effects the spell applies, read from its record.
    public IReadOnlyList<MagicEffectInstance> Effects => MagicEffectReader.Of(this);

    // True if the caster has the magicka to cast it.
    public bool CanCast(Actor caster) => caster.GetValue(ActorValue.Magicka) >= Cost;

    // Casts from `caster` at `target`: fails (changing nothing) if the caster
    // lacks the magicka, otherwise spends it and applies every effect to the
    // target. Returns whether it was cast.
    public bool Cast(Actor caster, Actor target)
    {
        if (!CanCast(caster)) return false;
        if (Cost > 0) caster.DamageValue(ActorValue.Magicka, Cost);
        foreach (MagicEffectInstance e in Effects) e.ApplyTo(target);
        return true;
    }

    // Casts a self-targeted spell (a power, ward or self-heal) on the caster.
    public bool Cast(Actor caster) => Cast(caster, caster);
}

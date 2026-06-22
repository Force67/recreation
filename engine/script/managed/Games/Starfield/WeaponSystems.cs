using System;

namespace Recreation.Games.Starfield;

// Starfield weapon damage: a weapon's base damage shaped by the shot's range and
// where it lands, paired with the channel it deals so DamageMitigation meets it
// with the right suit resistance. A hit holds full damage out to the weapon's
// effective range, then falls off linearly to a floor at its maximum range
// (ballistic weapons fall off the soonest, energy beams hold longer); a headshot
// (or a weak-spot hit) multiplies the impact. Pure math with no per-weapon state,
// so a mod resolves a shot's damage or previews a build; the curves are the
// vanilla feel, tunable. The firing class is read from the weapon's keywords via
// StarfieldEquipment.
public static class WeaponSystems
{
    // The fraction of base damage a shot keeps once it is past maximum range
    // (a distant shot still stings, it does not whiff to zero).
    public static float FalloffFloor { get; set; } = 0.4f;

    // A headshot deals this multiple of the body-shot damage (vanilla ~2x).
    public static float HeadshotMultiplier { get; set; } = 2f;

    // The damage left after range falloff: full inside `effectiveRange`, then a
    // linear ramp down to FalloffFloor at `maxRange`.
    public static float AfterFalloff(float baseDamage, float distance,
                                     float effectiveRange, float maxRange)
    {
        if (baseDamage <= 0f) return 0f;
        if (distance <= effectiveRange || maxRange <= effectiveRange) return baseDamage;
        float t = Math.Clamp((distance - effectiveRange) / (maxRange - effectiveRange), 0f, 1f);
        return baseDamage * (1f - (1f - FalloffFloor) * t);
    }

    // The default effective/maximum range a firing class holds, in game units;
    // energy weapons keep full damage farther than ballistics.
    public static (float Effective, float Max) RangeFor(StarfieldWeaponClass weaponClass) =>
        weaponClass switch
        {
            StarfieldWeaponClass.Laser => (2500f, 5000f),
            StarfieldWeaponClass.Particle => (3000f, 6000f),
            StarfieldWeaponClass.Electromagnetic => (1500f, 3000f),
            _ => (1500f, 4000f),  // ballistic and unclassified
        };

    // A shot's damage from `baseDamage` at `distance`, with an optional headshot,
    // using the firing class's default range band.
    public static float ShotDamage(float baseDamage, StarfieldWeaponClass weaponClass,
                                   float distance, bool headshot = false)
    {
        (float effective, float max) = RangeFor(weaponClass);
        float damage = AfterFalloff(baseDamage, distance, effective, max);
        return headshot ? damage * HeadshotMultiplier : damage;
    }
}

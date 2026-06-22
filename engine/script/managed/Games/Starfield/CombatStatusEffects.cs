namespace Recreation.Games.Starfield;

// A weapon- or ammo-applied status a Starfield hit can inflict on its target,
// the combat counterpart of the environmental afflictions. Each carries the
// affliction it lays on and a damage channel it is tied to, so an Electromagnetic
// weapon staggers, an incendiary round burns, and a corrosive round eats armor.
public readonly struct OnHitStatus(string id, string actorValue, float magnitude,
                                   StarfieldDamageType channel)
{
    public string Id { get; } = id;
    public string ActorValue { get; } = actorValue;
    public float Magnitude { get; } = magnitude;
    public StarfieldDamageType Channel { get; } = channel;

    // The affliction this status applies through the Afflictions registry.
    public Affliction AsAffliction() => new(Id, ActorValue, Magnitude);
}

// Weapon-applied combat status effects: the bridge from a landed hit to the
// Afflictions registry, so a status-carrying weapon or round inflicts its
// condition the same way an environmental hazard does. A pure pass-through to
// Afflictions (which holds the per-actor state and reverts the drain on cure),
// exposed as one ApplyOnHit call the combat code invokes when a hit connects.
// The catalogue below is the vanilla feel; the magnitudes are tunable.
public static class CombatStatusEffects
{
    // The stagger an Electromagnetic hit lays on, sapping the target's speed.
    public static OnHitStatus Staggered { get; set; } =
        new("Staggered", ActorValue.SpeedMult, 30f, StarfieldDamageType.Electromagnetic);

    // The burn an incendiary round lays on, draining Health over the carry.
    public static OnHitStatus Burning { get; set; } =
        new("Burning", ActorValue.Health, 10f, StarfieldDamageType.Energy);

    // Applies `status` to `target` through the Afflictions registry. Returns true
    // when the status took hold (false when the target already carries it, the
    // registry's own no-double-stack rule).
    public static bool ApplyOnHit(Actor target, OnHitStatus status) =>
        Afflictions.Contract(target, status.AsAffliction());

    // Clears a status from `target`, reverting just its drain. Returns true when
    // one was lifted.
    public static bool Clear(Actor target, OnHitStatus status) =>
        Afflictions.Cure(target, status.Id);
}

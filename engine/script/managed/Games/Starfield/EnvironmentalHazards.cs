using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// The kind of environmental hazard a world or location subjects the player to.
public enum SpaceHazard
{
    None,
    Thermal,
    Corrosive,
    Radiation,
    Airborne,
}

// How exposed to the active hazard the player is, in coarse stages the HUD and
// other systems react to.
public enum HazardStage
{
    Safe,
    Exposed,
    Critical,
}

// Raised when hazard exposure crosses into a new stage (or the hazard changes).
public readonly struct HazardStageChanged(SpaceHazard hazard, HazardStage stage) : IGameEvent
{
    public SpaceHazard Hazard { get; } = hazard;
    public HazardStage Stage { get; } = stage;
}

// Planet and location environmental hazards, a GameBehaviour that builds toward a
// hazard-specific affliction while the player stands in a thermal, corrosive,
// radioactive or airborne-toxin field with their suit unsealed. A sealed suit, or
// leaving the field, lets the exposure recover. Reaching full exposure inflicts
// the matching affliction through the Afflictions registry and recovering to safe
// cures just that one, so the cascade hazard -> affliction mirrors O2 -> hypoxia.
public sealed class EnvironmentalHazards : GameBehaviour
{
    // Exposure gained per second in an unsealed hazard, and shed per second when
    // safe (sealed or out of the field).
    public float ExposurePerSecond { get; set; } = 20f;
    public float RecoveryPerSecond { get; set; } = 25f;

    // Stage thresholds on the 0-100 scale.
    public float ExposedAt { get; set; } = 30f;
    public float CriticalAt { get; set; } = 100f;

    // The hazard the player currently stands in (None when safe) and whether the
    // suit is sealed against it. A live hazard volume sets these; the test sets
    // them directly, the simplest signal like OxygenCo2.Exerting.
    public SpaceHazard Hazard { get; set; }
    public bool Sealed { get; set; }

    public float Exposure { get; private set; }

    public HazardStage Stage =>
        Exposure >= CriticalAt ? HazardStage.Critical :
        Exposure >= ExposedAt ? HazardStage.Exposed : HazardStage.Safe;

    private SpaceHazard _lastHazard;
    private HazardStage _lastStage = HazardStage.Safe;
    private Affliction? _active;

    private bool Threatened => Hazard != SpaceHazard.None && !Sealed;

    protected override void OnUpdate(float deltaTime)
    {
        if (deltaTime <= 0f) return;
        Adjust(Threatened ? ExposurePerSecond * deltaTime : -RecoveryPerSecond * deltaTime);
    }

    private void Adjust(float delta)
    {
        Exposure = Math.Clamp(Exposure + delta, 0f, CriticalAt);

        if (Stage == HazardStage.Critical && _active == null && Hazard != SpaceHazard.None)
        {
            Affliction affliction = AfflictionFor(Hazard);
            if (Afflictions.Contract(Game.Player, affliction)) _active = affliction;
        }
        else if (Stage == HazardStage.Safe && _active is { } carried)
        {
            Afflictions.Cure(Game.Player, carried.Id);
            _active = null;
        }

        if (Stage == _lastStage && Hazard == _lastHazard) return;
        _lastStage = Stage;
        _lastHazard = Hazard;
        EventBus.Publish(new HazardStageChanged(Hazard, Stage));
    }

    // The affliction each hazard inflicts at full exposure. All sap Health for now;
    // a later pass can split them across resistance-specific actor values.
    private static Affliction AfflictionFor(SpaceHazard hazard) => hazard switch
    {
        SpaceHazard.Thermal => new Affliction("ThermalBurns", ActorValue.Health, 15f),
        SpaceHazard.Corrosive => new Affliction("ChemicalBurns", ActorValue.Health, 15f),
        SpaceHazard.Radiation => new Affliction("RadiationPoisoning", ActorValue.Health, 25f),
        SpaceHazard.Airborne => new Affliction("LungDamage", ActorValue.Health, 20f),
        _ => default,
    };
}

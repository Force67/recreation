using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// How much breathable oxygen the player has left, in coarse stages the HUD and
// other systems react to.
public enum OxygenStage
{
    Full,
    Low,
    Empty,
}

// How saturated the player is with carbon dioxide, the debt that builds once the
// oxygen runs out.
public enum CarbonDioxideStage
{
    Clear,
    Building,
    Maxed,
}

// Raised when oxygen crosses into a new stage.
public readonly struct OxygenStageChanged(OxygenStage stage) : IGameEvent
{
    public OxygenStage Stage { get; } = stage;
}

// Raised when carbon dioxide crosses into a new stage.
public readonly struct CarbonDioxideStageChanged(CarbonDioxideStage stage) : IGameEvent
{
    public CarbonDioxideStage Stage { get; } = stage;
}

// Starfield's signature O2/CO2 survival loop, a GameBehaviour that runs the whole
// mechanic in managed code. Exertion (a sprint flag, over-encumbrance, or being in
// combat) spends oxygen; once oxygen is gone the player accrues a carbon-dioxide
// debt that decays again as soon as oxygen returns. Both meters are kept here as
// managed state rather than actor values, since the engine models no O2/CO2 stat.
// Sustained maxed CO2 inflicts the Hypoxia affliction (the Afflictions registry holds
// against the player's Health) and clearing the debt cures it, so the cascade
// O2 -> CO2 -> affliction is real. Tunable from Starfield.json; it announces stage
// changes on the event bus and otherwise leaves the consequences to subscribers.
public sealed class OxygenCo2 : GameBehaviour
{
    // Oxygen spent per real second while exerting, and regained per second while at
    // rest. Tuned in seconds (not game hours) because exertion is a moment-to-moment
    // action, unlike the slow drift of hunger.
    public float DrainPerSecond { get; set; } = 12f;
    public float RegenPerSecond { get; set; } = 18f;
    // Oxygen restored by consuming one oxygen-source item.
    public float RestorePerSource { get; set; } = 60f;

    // CO2 accrued per second while oxygen is empty, and shed per second once
    // oxygen is available again.
    public float CarbonDioxideRisePerSecond { get; set; } = 10f;
    public float CarbonDioxideDecayPerSecond { get; set; } = 14f;

    // Stage thresholds on the 0-100 scales.
    public float OxygenLowAt { get; set; } = 35f;
    public float CarbonDioxideBuildingAt { get; set; } = 25f;
    public float CarbonDioxideMaxedAt { get; set; } = 100f;

    // How long (real seconds) CO2 must stay maxed before hypoxia sets in. A short
    // grace period so a brief sprint to empty does not immediately injure.
    public float HypoxiaAfterSeconds { get; set; } = 5f;

    // The keyword marking an item as an oxygen source to consume.
    public uint OxygenSourceKeyword { get; set; } = StarfieldForms.OxygenSourceKeyword;

    // The set-and-forget exertion input. The test (and, in a live session, a sprint
    // or encumbrance system) sets this; it is the simplest signal that decides
    // whether oxygen drains, mirroring how SurvivalNeeds keys feeding off the food
    // keyword rather than reaching for native player state.
    public bool Exerting { get; set; }

    // Whether the player is hauling over-mass cargo. The mass system sets this so
    // overburdening burns oxygen like sprinting does; kept separate from Exerting
    // so the two exertion sources combine rather than overwrite each other.
    public bool Overburdened { get; set; }

    // The hypoxia condition sustained maxed CO2 inflicts.
    public static readonly Affliction Hypoxia = new("Hypoxia", ActorValue.Health, 20f);

    public float Oxygen { get; private set; } = 100f;
    public float CarbonDioxide { get; private set; }

    public OxygenStage OxygenStage =>
        Oxygen <= 0f ? OxygenStage.Empty :
        Oxygen <= OxygenLowAt ? OxygenStage.Low : OxygenStage.Full;

    public CarbonDioxideStage CarbonDioxideStage =>
        CarbonDioxide >= CarbonDioxideMaxedAt ? CarbonDioxideStage.Maxed :
        CarbonDioxide >= CarbonDioxideBuildingAt ? CarbonDioxideStage.Building : CarbonDioxideStage.Clear;

    private OxygenStage _lastOxygenStage = OxygenStage.Full;
    private CarbonDioxideStage _lastCarbonDioxideStage = CarbonDioxideStage.Clear;
    private float _maxedSeconds;
    private bool _hypoxic;
    private EventBus.Subscription? _itemSubscription;

    protected override void OnStart() =>
        _itemSubscription = EventBus.Subscribe<ItemAdded>(OnItemAdded);

    protected override void OnDestroy() => _itemSubscription?.Dispose();

    protected override void OnUpdate(float deltaTime)
    {
        if (deltaTime <= 0f) return;

        // Exertion drains oxygen: a manual sprint flag, hauling over-mass, or being
        // in a fight (the live source that needs no external wiring, so the loop
        // actually runs during play). Otherwise it regenerates at rest.
        if (Exerting || Overburdened || Game.Player.IsInCombat) AdjustOxygen(-DrainPerSecond * deltaTime);
        else if (Oxygen < 100f) AdjustOxygen(RegenPerSecond * deltaTime);

        // CO2 only builds once oxygen is gone; with air to breathe it decays.
        if (Oxygen <= 0f) AdjustCarbonDioxide(CarbonDioxideRisePerSecond * deltaTime);
        else if (CarbonDioxide > 0f) AdjustCarbonDioxide(-CarbonDioxideDecayPerSecond * deltaTime);

        UpdateHypoxia(deltaTime);

        // Surface the breath meters on the HUD: oxygen (cyan) always, CO2 (amber)
        // only while it is rising, so the survival state reads at a glance.
        Hud.Gauge("oxygen", Oxygen / 100f, "Oxygen", 0x5ad6f0ffu);
        if (CarbonDioxide > 0f) Hud.Gauge("co2", CarbonDioxide / 100f, "CO2", 0xe0a23cffu);
        else Hud.ClearGauge("co2");
    }

    private void OnItemAdded(ItemAdded e)
    {
        if (e.ContainerHandle != Game.Player.Handle) return;
        if (!e.Item.HasKeyword(Game.GetForm(OxygenSourceKeyword))) return;
        AdjustOxygen(RestorePerSource * e.Count);
    }

    // Tops oxygen back to full and clears the carbon-dioxide debt, the way a rest
    // recovers the breath meters. Fires the stage events the change crosses.
    public void Restore()
    {
        AdjustOxygen(100f - Oxygen);
        AdjustCarbonDioxide(-CarbonDioxide);
    }

    private void AdjustOxygen(float delta)
    {
        Oxygen = Math.Clamp(Oxygen + delta, 0f, 100f);
        if (OxygenStage == _lastOxygenStage) return;
        _lastOxygenStage = OxygenStage;
        EventBus.Publish(new OxygenStageChanged(OxygenStage));
    }

    private void AdjustCarbonDioxide(float delta)
    {
        CarbonDioxide = Math.Clamp(CarbonDioxide + delta, 0f, CarbonDioxideMaxedAt);
        if (CarbonDioxideStage == _lastCarbonDioxideStage) return;
        _lastCarbonDioxideStage = CarbonDioxideStage;
        EventBus.Publish(new CarbonDioxideStageChanged(CarbonDioxideStage));
    }

    // Inflicts hypoxia once CO2 has been maxed long enough, and cures it as soon as
    // the debt eases. The Afflictions registry holds the Health drain; this only
    // decides when to contract and lift it.
    private void UpdateHypoxia(float deltaTime)
    {
        if (CarbonDioxideStage == CarbonDioxideStage.Maxed)
        {
            _maxedSeconds += deltaTime;
            if (!_hypoxic && _maxedSeconds >= HypoxiaAfterSeconds)
                _hypoxic = Afflictions.Contract(Game.Player, Hypoxia);
            return;
        }

        _maxedSeconds = 0f;
        if (_hypoxic)
        {
            Afflictions.Cure(Game.Player, Hypoxia.Id);
            _hypoxic = false;
        }
    }
}

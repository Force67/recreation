using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// How irradiated the player is, in coarse stages the HUD and other systems react
// to. Fallout 4 maps rads onto worsening conditions (Minor, Advanced, Critical
// radiation sickness); these are the thresholds those map to.
public enum RadiationStage
{
    Clean,
    Minor,
    Advanced,
    Critical,
}

// Raised when accumulated rads cross into a new stage.
public readonly struct RadiationStageChanged(RadiationStage stage, float rads) : IGameEvent
{
    public RadiationStage Stage { get; } = stage;
    public float Rads { get; } = rads;
}

// Fallout 4's signature radiation stat as managed soft logic. Rads accumulate from
// the world (a hot zone, eating raw food, drinking irradiated water) and each rad
// shaves the same amount off the player's effective max Health, exactly the way the
// game eats into the health bar from the right. The drain is held as a single
// Effect that is re-sized as rads change, so it composes with other Health modifiers
// and unwinds cleanly. RadAway (or sleeping in a clean bed) removes rads and restores
// the cap.
//
// The engine models no rad stat, so this is the authority: a hot field is set as a
// flag (RadsPerSecond > 0 while Irradiated, like OxygenCo2.Exerting), consuming a
// RadAway-keyworded aid item clears rads, and the public Add/Cure API lets a debug
// panel or future hazard volume drive it. It announces stage changes on the bus and
// leaves the consequences to subscribers (notifications).
public sealed class Radiation : GameBehaviour
{
    // Rads gained per real second while standing in a radioactive field. Tuned in
    // seconds because exposure is moment-to-moment, like OxygenCo2's drain.
    public float RadsPerSecond { get; set; } = 8f;
    // Rads removed by consuming one RadAway-keyworded item.
    public float CurePerDose { get; set; } = 300f;

    // Stage thresholds on the 0..MaxRads scale. Fallout's bands are roughly even
    // quarters of the bar; these mirror that shape.
    public float MinorAt { get; set; } = 200f;
    public float AdvancedAt { get; set; } = 500f;
    public float CriticalAt { get; set; } = 800f;
    // The cap: at max rads the whole 1000-point band of Health is eaten away.
    public float MaxRads { get; set; } = 1000f;

    // The keyword marking an item as a radiation cure (RadAway).
    public uint RadAwayKeyword { get; set; } = FalloutForms.RadAwayKeyword;

    // The set-and-forget exposure input. A live rad volume (or the debug panel)
    // sets this; while true rads climb at RadsPerSecond. The simplest signal that
    // decides whether rads accumulate, mirroring OxygenCo2.Exerting.
    public bool Irradiated { get; set; }

    public float Rads { get; private set; }

    public RadiationStage Stage =>
        Rads >= CriticalAt ? RadiationStage.Critical :
        Rads >= AdvancedAt ? RadiationStage.Advanced :
        Rads >= MinorAt ? RadiationStage.Minor : RadiationStage.Clean;

    private RadiationStage _lastStage = RadiationStage.Clean;
    private Effect? _healthCap;  // the held Health drain, re-sized as rads change
    private EventBus.Subscription? _itemSubscription;

    protected override void OnStart() =>
        _itemSubscription = EventBus.Subscribe<ItemAdded>(OnItemAdded);

    protected override void OnDestroy()
    {
        _itemSubscription?.Dispose();
        ClearHealthCap();
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (deltaTime > 0f && Irradiated) Adjust(RadsPerSecond * deltaTime);

        // Persistent rads bar (sickly green). Rads linger until cured, so the
        // meter shows whenever any have accumulated, not only in a hot field.
        if (Rads > 0f) Hud.Gauge("rads", Rads / MaxRads, "Rads", 0x6fb04dffu);
        else Hud.ClearGauge("rads");
    }

    private void OnItemAdded(ItemAdded e)
    {
        if (e.ContainerHandle != Game.Player.Handle) return;
        if (!e.Item.HasKeyword(Game.GetForm(RadAwayKeyword))) return;
        Cure(CurePerDose * e.Count);
    }

    // Adds rads (the world or food irradiating the player); negative removes them.
    // Public so a rad volume or the debug panel can dose the player directly.
    public void Add(float rads) => Adjust(rads);

    // Removes `rads`, the way a RadAway dose or a clean rest cleans the player up.
    public void Cure(float rads) => Adjust(-Math.Abs(rads));

    // Removes every rad, restoring the full Health cap (a doctor's RadAway course).
    public void Decontaminate() => Adjust(-Rads);

    private void Adjust(float delta)
    {
        Rads = Math.Clamp(Rads + delta, 0f, MaxRads);
        ApplyHealthCap();
        if (Stage == _lastStage) return;
        _lastStage = Stage;
        EventBus.Publish(new RadiationStageChanged(Stage, Rads));
    }

    // Holds a single Health drain equal to the current rads, replacing the old one
    // so the cap tracks rads exactly rather than stacking dose on dose.
    private void ApplyHealthCap()
    {
        ClearHealthCap();
        if (Rads <= 0f) return;
        _healthCap = Effects.Apply(Game.Player, ActorValue.Health, -Rads, durationSeconds: 0f);
    }

    private void ClearHealthCap()
    {
        if (_healthCap == null) return;
        Effects.Remove(_healthCap);
        _healthCap = null;
    }
}

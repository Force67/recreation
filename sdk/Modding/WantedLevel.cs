using System;
using Recreation;

namespace Recreation.Modding;

// The coarse "are the guards after me" heat the player builds by committing
// crimes, in four bands the rest of the social layer reads. It is the cross-game
// twin of a per-hold bounty or a jurisdiction ledger: those track who is owed
// gold, this tracks how openly hunted the player is right now and surfaces it as a
// standing HUD bar.
public enum WantedBand
{
    Clear = 0,       // no heat: the player draws no attention
    Suspected = 1,   // a minor crime: side-eyes, no pursuit
    Wanted = 2,      // a reported crime: guards will confront on sight
    Hunted = 3,      // a string of crimes: actively pursued
}

// Raised when the player's heat crosses into a new band. The signature state
// change the guard layer, music and NPC reactions hook, so they react to a band
// change rather than polling the raw points.
public readonly struct WantedBandChanged(WantedBand from, WantedBand to) : IGameEvent
{
    public WantedBand From { get; } = from;
    public WantedBand To { get; } = to;

    // True when the band rose (a crime), false when it fell (the heat cooled).
    public bool Escalated => To > From;
}

// A generic crime cue any game's crime system can publish, so the wanted meter is
// fed without WantedLevel depending on Skyrim's Crime or Starfield's Bounties. A
// witness, a stealth system or a quest raises it with the severity of the deed
// (the bounty-gold scale: a pickpocket is small, a murder large). WantedLevel and
// the bounty ledgers both subscribe, the way two systems share one event.
public readonly struct CrimeReported(int severity) : IGameEvent
{
    public int Severity { get; } = severity;
}

// A unified wanted/heat meter: a cross-game GameBehaviour that raises the player's
// heat when a crime is reported (via the CrimeReported event or the Report API a
// crime system calls), bleeds it back down every frame so it cools when the player
// lies low, classifies it into the four WantedBands, and surfaces it as a standing
// HUD gauge plus a band-change notification. It owns only the heat number; the
// bounty ledgers (who is owed what) stay with each game. Self-contained runtime
// state, reset on teardown.
//
// Driven two ways at once: per-frame OnUpdate handles the decay and the gauge, and
// the CrimeReported subscription (plus the public Report) drives it up. That keeps
// it event-fed but clock-cooled, exactly like the survival meters.
public sealed class WantedLevel : GameBehaviour
{
    // The id of the HUD gauge this owns, the standing heat bar.
    public const string GaugeId = "wanted";

    // The heat at which each band begins. Below SuspectedAt the player is Clear.
    public float SuspectedAt { get; set; } = 1f;
    public float WantedAt { get; set; } = 40f;
    public float HuntedAt { get; set; } = 1000f;

    // The heat a full Hunted bar represents, the gauge's 1.0 point. Heat above this
    // is held (the bar pins full) so a spree does not silently overflow.
    public float MaxHeat { get; set; } = 2000f;

    // Heat bled off per real second while no fresh crime lands. The signature cool
    // down: lie low and the guards lose interest.
    public float DecayPerSecond { get; set; } = 2f;

    // The current heat and its band.
    public float Heat { get; private set; }
    public WantedBand Band { get; private set; } = WantedBand.Clear;

    public bool IsWanted => Band >= WantedBand.Wanted;

    private EventBus.Subscription? _crimeSub;

    protected override void OnStart()
    {
        // Any crime system publishes CrimeReported; we accumulate its severity.
        _crimeSub = EventBus.Subscribe<CrimeReported>(e => Report(e.Severity));
        PushGauge();
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (Heat <= 0f) return;
        Heat = MathF.Max(0f, Heat - DecayPerSecond * deltaTime);
        Reclassify();
        PushGauge();
    }

    protected override void OnDestroy()
    {
        _crimeSub?.Dispose();
        _crimeSub = null;
        Hud.ClearGauge(GaugeId);
    }

    // Adds `severity` heat (the bounty-gold scale) and reclassifies. A non-positive
    // severity is a no-op (a crime no one noticed). The entry point a crime system
    // calls directly, the imperative twin of publishing CrimeReported.
    public void Report(int severity)
    {
        if (severity <= 0) return;
        Heat = MathF.Min(MaxHeat, Heat + severity);
        Reclassify();
        PushGauge();
    }

    // Wipes the heat outright (the bounty was paid, the player turned themselves
    // in, a pardon). Drops the band to Clear and fires the change.
    public void Clear()
    {
        if (Heat == 0f && Band == WantedBand.Clear) return;
        Heat = 0f;
        Reclassify();
        PushGauge();
    }

    // The band a given heat falls in, the shared classifier so callers and tests
    // agree on the thresholds.
    public WantedBand BandFor(float heat)
    {
        if (heat >= HuntedAt) return WantedBand.Hunted;
        if (heat >= WantedAt) return WantedBand.Wanted;
        if (heat >= SuspectedAt) return WantedBand.Suspected;
        return WantedBand.Clear;
    }

    // Recomputes the band from the current heat and announces a crossing once.
    private void Reclassify()
    {
        WantedBand next = BandFor(Heat);
        if (next == Band) return;
        WantedBand previous = Band;
        Band = next;
        EventBus.Publish(new WantedBandChanged(previous, next));
        Notify(previous, next);
    }

    private void Notify(WantedBand from, WantedBand to)
    {
        string? line = to switch
        {
            WantedBand.Clear => "Wanted: cleared",
            WantedBand.Suspected => "Wanted: suspected",
            WantedBand.Wanted => "Wanted: guards are after you",
            WantedBand.Hunted => "Wanted: hunted",
            _ => null,
        };
        if (line != null) Debug.Notification(line);
    }

    // Pushes the heat as a 0..1 fill, or clears the bar when fully cool.
    private void PushGauge()
    {
        if (Heat <= 0f) Hud.ClearGauge(GaugeId);
        else Hud.Gauge(GaugeId, MathF.Min(1f, Heat / MaxHeat), "Wanted", 0xd23c3cffu);
    }
}

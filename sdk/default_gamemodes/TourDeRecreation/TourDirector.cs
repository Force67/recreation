using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.TourDeRecreation;

// What a stop is handed: the card it narrates on, the world it acts on, and a
// little shared scratch for the things one stop sets up and the next uses.
public sealed class TourContext
{
    public required TourCard Card { get; init; }
    public required TourWorld World { get; init; }

    // Base forms stop 2 found and stop 3 clones. Empty in a world with nothing
    // resident nearby, which the stops handle rather than assume away.
    public List<Form> Forms { get; } = new();

    // Set by the hotkey stop when the player actually presses the key, so the
    // card can acknowledge it.
    public int Nudges { get; set; }
}

// Tour de Recreation: a guided demo that drives itself, written against nothing
// but the public modding SDK.
//
// That constraint is the demo. Every line of this gamemode is API any mod can
// call: it reads the world through Game/ObjectReference, changes it through
// PlaceAtMe/Position/Delete, drives the engine's own UI through Ui.Find, puts a
// gauge on the HUD through Hud.Gauge and takes a keypress through Hotkeys.Bind.
// Nothing here reaches into the engine, and nothing in the engine knows the tour
// exists beyond shipping the .ugui card it writes to.
//
// It is a GameBehaviour, so the host ticks it every frame; the stop list below
// is the whole script.
public sealed class TourDirector : GameBehaviour
{
    // Seconds to wait after the world comes up before the first card. The tour
    // opens on a world that has just streamed in, and starting mid-hitch reads
    // as a stutter rather than an opening.
    public float LeadIn { get; init; } = 3f;

    // The tour will not open on an empty view. Cells are still streaming when the
    // world reports ready, and a game's own start-up quests move the player
    // around for the first few seconds (Skyrim's warp the player through two
    // interiors before settling), so a fixed wait can easily open the card on a
    // patch of fog with nothing to demonstrate. Instead it waits for the player
    // to actually have neighbours, and gives up after Settle so a genuinely bare
    // spot still gets a tour rather than nothing at all.
    public int MinNeighbours { get; init; } = 6;
    public float Settle { get; init; } = 25f;

    // The ring the tour builds: how many props, how far out, how high.
    public int RingSize { get; init; } = 12;
    public float RingRadius { get; init; } = 420f;   // game units, ~6 m
    public float RingLift { get; init; } = 90f;      // ~1.3 m, about chest height
    public float RingBob { get; init; } = 34f;       // how far each prop rides up and down
    // How far to look for things to clone. Generous on purpose: a spawn point can
    // easily be a bare hillside with one neighbour inside 20 m, and a tour that
    // reports "nothing nearby" on open is a tour nobody watches twice.
    public float SearchRadius { get; init; } = 6000f;  // ~85 m
    // How much further out one press of the key pushes the ring.
    public float NudgeStep { get; init; } = 120f;

    // The key the tour hands the player. B is unbound by the default controls,
    // so pressing it during the tour cannot also do something else.
    public Key NudgeKey { get; init; } = Key.B;

    private readonly TourCard _card = new();
    private readonly TourWorld _world = new();
    private TourContext _context = null!;
    private List<TourStop> _stops = null!;
    private EventBus.Subscription? _nudge;

    private int _stop = -1;
    private float _elapsed;      // seconds inside the current stop
    private float _sinceStart;   // seconds since the behaviour started
    private float _sincePoll;    // seconds since the last "is there a world yet" check
    private int _neighbours;     // references found by that check
    private bool _running;
    private bool _finished;

    protected override void OnStart()
    {
        _context = new TourContext { Card = _card, World = _world };
        _stops = BuildStops();
        // The card has exactly as many stop marks as tour.ugui authors. Saying so
        // out loud beats a tour that silently stops advancing its marks.
        if (_stops.Count != TourCard.MarkCount)
            Debug.Trace($"[tour] {_stops.Count} stops but {TourCard.MarkCount} marks on the card");
        _nudge = Hotkeys.Bind(NudgeKey, OnNudge);
        Debug.Trace($"[tour] armed: {_stops.Count} stops, starting in {LeadIn:0.#}s");
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (_finished) return;

        if (!_running)
        {
            _sinceStart += deltaTime;
            if (!ReadyToOpen()) return;
            _running = true;
            _card.Show(true);
            Advance();
            return;
        }

        _elapsed += deltaTime;
        TourStop current = _stops[_stop];
        current.Tick?.Invoke(_context, _elapsed);
        if (_elapsed >= current.Seconds) Advance();
    }

    protected override void OnDestroy()
    {
        _nudge?.Dispose();
        _world.Clear();
        Hud.ClearGauge("tour");
        _card.Show(false);
    }

    // Whether there is a world worth touring yet: past the lead-in, and either
    // enough resident neighbours to work with or out of patience. Polled about
    // twice a second rather than every frame, since the proximity query walks
    // every registered reference.
    private bool ReadyToOpen()
    {
        if (_sinceStart < LeadIn) return false;
        _sincePoll += Time.DeltaTime;
        bool polled = _sincePoll >= 0.5f;
        if (polled)
        {
            _sincePoll = 0f;
            _neighbours = Game.Player.RefsNear(SearchRadius).Length;
        }
        if (_sinceStart >= Settle)
        {
            Debug.Trace($"[tour] opening after {Settle:0.#}s with {_neighbours} references nearby");
            return true;
        }
        if (!polled || _neighbours < MinNeighbours) return false;
        Debug.Trace($"[tour] world settled: {_neighbours} references nearby");
        return true;
    }

    // Close the running stop, open the next one, or finish.
    private void Advance()
    {
        if (_stop >= 0) _stops[_stop].Exit?.Invoke(_context);

        _stop++;
        _elapsed = 0f;
        if (_stop >= _stops.Count)
        {
            _finished = true;
            _card.Show(false);
            Debug.Notification("Tour de Recreation complete.");
            Debug.Trace("[tour] complete");
            return;
        }

        TourStop stop = _stops[_stop];
        _card.SetStop(_stop, _stops.Count, stop);
        Debug.Trace($"[tour] stop {_stop + 1}/{_stops.Count}: {stop.Title}");
        stop.Enter?.Invoke(_context);
    }

    private void OnNudge()
    {
        if (!_running || _finished) return;
        _context.Nudges++;
        // Push the ring out a little each press, so the key does something the
        // player can see rather than something they have to trust.
        _world.Orbit(_elapsed, RingRadius + _context.Nudges * NudgeStep, RingLift, 0f);
    }

    // ---- the tour itself ---------------------------------------------------
    // Seven stops, each one thing a mod can do, in the order that builds on the
    // last: read the world, change the world, drive the interface, take input,
    // and put everything back.
    private List<TourStop> BuildStops() => new()
    {
        new TourStop
        {
            Title = "One engine, many worlds",
            Line0 = "Everything you are about to see is a mod, running on the public SDK.",
            Line1 = "No engine code knows this tour exists.",
            Seconds = 10f,
            Enter = c =>
            {
                string primary = Domains.Primary?.Name ?? "no world";
                c.Card.SetStat($"{primary}  |  {Domains.Count} domain(s)  |  SDK {SdkInfo.Version}");
                Debug.Notification("Tour de Recreation");
            },
        },

        new TourStop
        {
            Title = "The world is data",
            Line0 = "Every object around you is a record the game shipped, streamed into cells.",
            Line1 = "A mod can ask what is nearby and get the live answer, this frame.",
            Seconds = 11f,
            Enter = c =>
            {
                NearbyRef[] near = TourWorld.Sorted(Game.Player.RefsNear(SearchRadius));
                c.Forms.Clear();
                c.Forms.AddRange(TourWorld.NearbyBaseForms(SearchRadius, 8));
                string nearest = near.Length > 0 ? near[0].Reference.BaseObject.Name : "nothing";
                if (string.IsNullOrEmpty(nearest)) nearest = "an unnamed marker";
                c.Card.SetStat($"{near.Length} references nearby  |  nearest: {nearest}");
            },
        },

        new TourStop
        {
            Title = "Anything can be placed",
            Line0 = "These are copies of what was already standing here, spawned just now.",
            Line1 = "Same call any mod makes: PlaceAtMe, then set a position.",
            Seconds = 11f,
            Enter = c =>
            {
                if (c.Forms.Count == 0)
                {
                    c.Card.SetStat("nothing resident nearby to copy -- walk into a town and re-run");
                    return;
                }
                int placed = c.World.PlaceRing(c.Forms, RingSize, RingRadius, RingLift);
                c.Card.SetStat($"{placed} placed from {c.Forms.Count} base form(s)");
            },
        },

        new TourStop
        {
            Title = "And what is placed is alive",
            Line0 = "The ring is being moved every single frame, from C#.",
            Line1 = "World references are objects a mod owns, not scenery it decorates around.",
            Seconds = 13f,
            Tick = (c, t) =>
            {
                c.World.Orbit(t * 0.9f, RingRadius, RingLift, RingBob);
                if (c.World.PlacedCount > 0)
                    c.Card.SetStat($"{c.World.PlacedCount} references moved this frame");
            },
        },

        new TourStop
        {
            Title = "The interface is yours",
            Line0 = "This card is not an engine screen: it is a mod writing widgets by name.",
            Line1 = "So is the gauge that just appeared, and the notice that came with it.",
            Seconds = 12f,
            Enter = _ => Debug.Notification("A mod put this notice here."),
            Tick = (c, t) =>
            {
                c.World.Orbit(t * 0.9f + 3f, RingRadius, RingLift, RingBob);
                float fill = 0.5f + 0.5f * MathF.Sin(t * 1.4f);
                Hud.Gauge("tour", fill, "Tour", 0xffffffffu);
                c.Card.SetStat($"gauge \"tour\" = {fill:0.00}");
            },
        },

        new TourStop
        {
            Title = "Your keys, your rules",
            Line0 = "A mod can take input directly. This one is listening right now.",
            Line1 = "Every press pushes the ring further out.",
            Hint = "B  --  push the ring out",
            Seconds = 13f,
            Tick = (c, t) =>
            {
                c.World.Orbit(t * 0.9f + 6f, RingRadius + c.Nudges * NudgeStep, RingLift, RingBob);
                c.Card.SetStat(c.Nudges == 0
                    ? "waiting for a press of B"
                    : $"{c.Nudges} press(es)  |  ring at {RingRadius + c.Nudges * NudgeStep:0} units");
            },
        },

        new TourStop
        {
            Title = "And none of it was written down",
            Line0 = "The copies are gone and the gauge is cleared. Your game files were only read.",
            Line1 = "Everything the tour did, it did at runtime, and any mod can do the same.",
            Seconds = 11f,
            Enter = c =>
            {
                c.World.Clear();
                Hud.ClearGauge("tour");
                c.Card.SetStat($"{c.World.PlacedCount} placed  |  0 files written");
                Debug.Notification("Nothing was written to your game.");
            },
        },
    };
}

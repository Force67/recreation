using System;

namespace Recreation.Games.TourDeRecreation;

// One stop on the tour: what the card says, how long it holds, and the work it
// does while it is up.
//
// Enter runs once when the stop opens, Tick every frame it is up (with seconds
// since the stop opened, so a stop can animate), Exit once when it closes. All
// three are optional: a stop that only narrates supplies none of them.
public sealed class TourStop
{
    public required string Title { get; init; }
    public required string Line0 { get; init; }
    public string Line1 { get; init; } = "";
    // What the player can press while this stop is up, shown at the foot of the
    // card. Empty means the tour is driving and there is nothing to do.
    public string Hint { get; init; } = "";
    public float Seconds { get; init; } = 11f;

    public Action<TourContext>? Enter { get; init; }
    public Action<TourContext, float>? Tick { get; init; }
    public Action<TourContext>? Exit { get; init; }
}

using Recreation;

namespace Recreation.Games.TourDeRecreation;

// The tour's narration card, driven through the SDK's widget API.
//
// This is the part of the tour that is itself a demonstration: runtime/ui/
// screens/tour.ugui declares the card and nothing in the engine writes to it.
// Every line of text, every stop mark and the card's own visibility are set from
// here, by a mod, through exactly the API any other mod has (Ui.Find + Widget).
//
// A dedicated server has no UI backend, so Ui.Available is false and every call
// below is an inert no-op; the tour still runs, it just narrates to the log.
public sealed class TourCard
{
    // Matches the number of stop marks tour.ugui authors. A tour longer than
    // this still runs; the marks simply stop advancing, which is why the count
    // is asserted at boot rather than silently clamped.
    public const int MarkCount = 7;

    public bool Available => Ui.Available;

    public void Show(bool visible)
    {
        Ui.Find("tour_card").Visible = visible;
        if (!visible)
        {
            // Leave the marks dark, so a second run of the tour starts clean
            // rather than inheriting the last run's lit ones.
            for (int i = 0; i < MarkCount; i++) Ui.Find($"tour_pipon{i}").Visible = false;
        }
    }

    // Write one stop onto the card. `index` is zero-based; the card counts from
    // one because that is how a person counts stops on a tour.
    public void SetStop(int index, int total, TourStop stop)
    {
        Ui.Find("tour_step").Text = $"Stop {index + 1:00} of {total:00}";
        Ui.Find("tour_title").Text = stop.Title;
        Ui.Find("tour_body0").Text = stop.Line0;
        Ui.Find("tour_body1").Text = stop.Line1;
        Ui.Find("tour_hint").Text = stop.Hint;
        Ui.Find("tour_stat").Text = "";
        for (int i = 0; i < MarkCount; i++) Ui.Find($"tour_pipon{i}").Visible = i <= index;
    }

    // The live readout at the foot of the card: whatever the running stop wants
    // to prove with a number (how many references it found, how many props it is
    // moving this frame).
    public void SetStat(string text) => Ui.Find("tour_stat").Text = text;
}

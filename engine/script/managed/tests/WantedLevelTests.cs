using System.Collections.Generic;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the unified wanted/heat meter: a reported crime raises the heat and bands
// it, the heat decays per frame and cools the band back down, band crossings fire
// an event and a notification, the meter surfaces as a HUD gauge, the CrimeReported
// event feeds it, and Clear wipes it.
public static class WantedLevelTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        var bands = new List<WantedBandChanged>();
        EventBus.Subscribe<WantedBandChanged>(bands.Add);

        var wanted = new WantedLevel { DecayPerSecond = 10f };
        ModHost.Register(wanted);

        // Starts clear, with no gauge shown.
        check.Equal("starts clear", WantedBand.Clear, wanted.Band);
        check.That("no gauge while clear", !fake.Gauges.ContainsKey(WantedLevel.GaugeId));

        // A small crime makes the player Suspected and raises the gauge.
        wanted.Report(5);
        check.Equal("a small crime is suspected", WantedBand.Suspected, wanted.Band);
        check.That("the wanted gauge is shown", fake.Gauges.ContainsKey(WantedLevel.GaugeId));
        check.Equal("one band change so far", 1, bands.Count);
        check.That("suspected was notified", fake.Notifications.Contains("Wanted: suspected"));

        // A bigger crime crosses into Wanted.
        wanted.Report(50);  // heat now 55, over WantedAt (40)
        check.Equal("a reported crime is wanted", WantedBand.Wanted, wanted.Band);
        check.That("now wanted", wanted.IsWanted);
        check.That("wanted was notified", fake.Notifications.Contains("Wanted: guards are after you"));

        // The gauge fraction tracks heat / MaxHeat.
        check.Equal("gauge fraction is heat over max", 55f / wanted.MaxHeat,
                    fake.Gauges[WantedLevel.GaugeId]);

        // A spree pins the band at Hunted.
        wanted.Report(1000);  // heat 1055, over HuntedAt (1000)
        check.Equal("a spree is hunted", WantedBand.Hunted, wanted.Band);

        // Heat decays per frame and cools the band; ticking long enough clears it.
        int changesBefore = bands.Count;
        for (int i = 0; i < 200; i++) ModHost.Tick(1f);  // 200s * 10/s = 2000 heat bled
        check.Equal("heat decayed to zero", 0f, wanted.Heat);
        check.Equal("cooled back to clear", WantedBand.Clear, wanted.Band);
        check.That("cooling fired band changes", bands.Count > changesBefore);
        check.That("the gauge cleared when cool", !fake.Gauges.ContainsKey(WantedLevel.GaugeId));
        check.That("clear was notified", fake.Notifications.Contains("Wanted: cleared"));

        // The CrimeReported event feeds the meter too.
        EventBus.Publish(new CrimeReported(50));
        check.Equal("a published crime is wanted", WantedBand.Wanted, wanted.Band);

        // A non-positive crime is a no-op.
        float heatBefore = wanted.Heat;
        wanted.Report(0);
        wanted.Report(-10);
        check.Equal("a harmless crime does nothing", heatBefore, wanted.Heat);

        // Clear wipes the heat and drops to Clear.
        wanted.Clear();
        check.Equal("clear wipes the heat", 0f, wanted.Heat);
        check.Equal("clear drops the band", WantedBand.Clear, wanted.Band);

        // BandFor is the shared classifier callers and the meter agree on.
        check.Equal("BandFor at threshold is suspected", WantedBand.Suspected, wanted.BandFor(1f));
        check.Equal("BandFor below threshold is clear", WantedBand.Clear, wanted.BandFor(0.5f));
        check.Equal("BandFor high is hunted", WantedBand.Hunted, wanted.BandFor(5000f));

        // Teardown disposes the subscription and clears the gauge.
        ModHost.Unregister(wanted);
        int countAfter = bands.Count;
        EventBus.Publish(new CrimeReported(100));
        check.Equal("no longer reacts after teardown", countAfter, bands.Count);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}

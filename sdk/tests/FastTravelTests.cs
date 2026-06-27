using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the reason-based fast-travel gate and the indoor lock that drives it,
// the coordination that lets several systems gate fast travel without fighting.
public static class FastTravelTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        FastTravel.Clear();

        check.That("open by default", !FastTravel.IsBlocked && fake.FastTravelEnabled);

        FastTravel.Block("combat");
        check.That("one reason closes it", FastTravel.IsBlocked && !fake.FastTravelEnabled);

        FastTravel.Block("indoors");           // a second reason
        FastTravel.Unblock("combat");          // first reason clears
        check.That("stays closed while a reason remains",
                   FastTravel.IsBlocked && !fake.FastTravelEnabled);

        FastTravel.Unblock("indoors");         // last reason clears
        check.That("reopens when the last reason clears",
                   !FastTravel.IsBlocked && fake.FastTravelEnabled);

        // The indoor lock drives the gate from LocationChanged.
        ModHost.Shutdown();
        EventBus.Clear();
        ModHost.Register(new IndoorFastTravelLock());

        EventBus.Publish(new LocationChanged(0x100, isInterior: true));
        check.That("indoors closes fast travel", !fake.FastTravelEnabled);
        EventBus.Publish(new LocationChanged(0, isInterior: false));
        check.That("outdoors reopens it", fake.FastTravelEnabled);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}

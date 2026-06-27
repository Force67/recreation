using Recreation;

namespace Recreation.Tests;

// Exercises the managed half of the ultragui C# scripting backend: [UiHandler]
// discovery, name dispatch, and the Ui.After timer. The native widget ops are not
// bound here (no engine), so handlers that touch widgets safely no-op; the pexrun
// hosttest covers the native boundary.
public static class UiTests
{
    public static void Run(Check check)
    {
        // Programmatic registration + dispatch round trip.
        bool fired = false;
        ulong got = 0;
        Ui.On("ut_handler", w => { fired = true; got = w.Handle; });
        int claimed = Ui.Dispatch("ut_handler", 0x2a);
        check.Equal("dispatch invokes the handler", 1, claimed);
        check.That("handler ran", fired);
        check.Equal("handler receives the widget handle", 0x2aUL, got);

        // Unknown handler is reported as unclaimed.
        check.Equal("unknown handler not claimed", 0, Ui.Dispatch("ut_nope", 0));

        // Off removes it.
        Ui.Off("ut_handler");
        check.Equal("removed handler not claimed", 0, Ui.Dispatch("ut_handler", 0));

        // A throwing handler is isolated but still counts as claimed.
        Ui.On("ut_throw", _ => throw new System.InvalidOperationException("boom"));
        check.Equal("throwing handler still claimed", 1, Ui.Dispatch("ut_throw", 0));
        Ui.Off("ut_throw");

        // Ui.After fires only once enough UI time has elapsed.
        int ticks = 0;
        Ui.After(1.0, () => ticks++);
        Ui.Tick(0.5f);
        check.Equal("timer not yet due", 0, ticks);
        Ui.Tick(0.6f);
        check.Equal("timer fires once due", 1, ticks);
        Ui.Tick(5.0f);
        check.Equal("timer does not refire", 1, ticks);

        // Attribute discovery: scanning an assembly registers its [UiHandler]
        // methods, so dispatching one by name is claimed.
        Ui.ScanAssembly(typeof(UiTests).Assembly);
        check.Equal("[UiHandler] methods are discovered", 1, Ui.Dispatch("on_ut_scanned", 0));
        Ui.Off("on_ut_scanned");
    }

    // Discovered by the ScanAssembly check above; the body is irrelevant (no
    // widget ops are bound here), only that the scan finds and registers it.
    [UiHandler]
    private static void on_ut_scanned(Widget w) { }
}

using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the scriptable HUD kit headless: notifications expire, prompts manage a keyed set, and the menu navigates/selects/toggles.
public static class HudKitTests
{
    public static void Run(Check check)
    {
        Native.Backend = new FakeBackend();
        EventBus.Clear();              // isolate our FrameUpdate from other suites
        HudKit.Bind(NetRole.Standalone);

        // --- Notify: a shown toast is recorded, and frame time expires it ---
        Notify.Show("Saved", NoticeKind.Success, 4f);
        check.Equal("show records one active notice", 1, Notify.Active.Count);
        check.Equal("notice carries its kind", NoticeKind.Success, Notify.Active[0].Kind);
        EventBus.Publish(new FrameUpdate(5f));   // past its 4s lifetime
        check.Equal("a frame past its lifetime expires the notice", 0, Notify.Active.Count);

        // A notice survives a short frame and falls off once its time is spent.
        Notify.Show("Ping");                     // defaults: Info, 4s
        EventBus.Publish(new FrameUpdate(1f));
        check.Equal("notice still active after a short frame", 1, Notify.Active.Count);
        EventBus.Publish(new FrameUpdate(4f));
        check.Equal("notice gone once its time elapses", 0, Notify.Active.Count);

        // --- Prompts: show, idempotent update, get, hide ---
        Prompts.Show("door", "Open Door", "E");
        Prompts.Show("chest", "Loot", "F");
        check.Equal("both prompts are visible", 2, Prompts.Active.Count);
        check.Equal("get returns the prompt", "Open Door", Prompts.Get("door")!.Label);

        Prompts.Show("door", "Close Door");      // same id: update, not duplicate
        check.Equal("re-show updates rather than duplicates", 2, Prompts.Active.Count);
        check.Equal("re-show updated the label", "Close Door", Prompts.Get("door")!.Label);
        check.Equal("re-show reset the key to default", "E", Prompts.Get("door")!.Key);

        Prompts.Hide("door");
        check.Equal("hiding drops it from the active set", 1, Prompts.Active.Count);
        check.That("hidden prompt is marked not visible", !Prompts.Get("door")!.Visible);

        // --- Menu: navigation wraps, select fires, toggle flips ---
        bool refuelFired = false;
        bool? lightsState = null;
        var menu = new Menu("Garage");
        menu.Add("Repair", () => { });
        menu.Add("Refuel", () => refuelFired = true);
        menu.AddToggle("Lights", false, on => lightsState = on);

        menu.Open();
        check.That("opening tracks the menu as active", ReferenceEquals(HudKit.ActiveMenu, menu));
        check.Equal("focus starts at the first row", 0, menu.Selected);

        menu.MoveDown();
        check.Equal("move down advances the focus", 1, menu.Selected);
        menu.Select();
        check.That("select invokes the focused item's action", refuelFired);

        menu.MoveDown();                         // -> row 2, the toggle
        menu.Select();
        check.That("selecting a toggle flips and reports it", lightsState == true);

        menu.MoveDown();                         // 2 -> wraps to 0
        check.Equal("move down wraps past the last row", 0, menu.Selected);
        menu.MoveUp();                           // 0 -> wraps to last
        check.Equal("move up wraps past the first row", 2, menu.Selected);

        menu.Close();
        check.That("closing clears the active menu", HudKit.ActiveMenu == null);

        // --- Reset clears prompts, notices and the tracked menu ---
        Notify.Show("transient");
        Prompts.Show("again", "Activate");
        new Menu("Scratch").Open();
        HudKit.Reset();
        check.Equal("reset clears notices", 0, Notify.Active.Count);
        check.Equal("reset clears prompts", 0, Prompts.Active.Count);
        check.That("reset clears the active menu", HudKit.ActiveMenu == null);

        HudKit.Reset();
        EventBus.Clear();
        Native.Backend = null;
    }
}

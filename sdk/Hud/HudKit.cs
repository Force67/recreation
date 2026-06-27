using System;
using Recreation.Modding;

namespace Recreation.Net;

// Owns the per-frame tick that expires notices and tracks which menu is open so
// input can be routed to it. Notify/Prompts/Menu are passive stores. Bind is
// idempotent (it resets first).
public static class HudKit
{
    private static IDisposable? _frame;

    public static NetRole Role { get; private set; } = NetRole.Standalone;

    // The menu currently accepting navigation input, if any.
    public static Menu? ActiveMenu { get; internal set; }

    // Wire the HUD for a role. Only notices need the frame tick to age out; prompts
    // and menus are driven directly by the mod.
    public static void Bind(NetRole role)
    {
        Reset();
        Role = role;
        _frame = EventBus.Subscribe<FrameUpdate>(f => Notify.Tick(f.DeltaTime));
    }

    // Drop the frame hook and clear HUD state. Forgets the active menu without
    // touching the native (the backend may already be gone during shutdown).
    public static void Reset()
    {
        _frame?.Dispose();
        _frame = null;
        Prompts.Clear();
        Notify.Clear();
        ActiveMenu = null;
        Role = NetRole.Standalone;
    }
}

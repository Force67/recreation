using Recreation.Interop;

namespace Recreation;

// Diagnostics and on-screen messages, mirroring the Papyrus Debug script. The
// everyday way a mod talks to the player (Notification) or the log (Trace).
public static class Debug
{
    // A corner notification, the small status line Skyrim shows for pickups and
    // hints.
    public static void Notification(string message) => Global("Notification", message);

    // A log line (papyrus.log equivalent), for development output.
    public static void Trace(string message) => Global("Trace", message);

    // A blocking message box. The engine surfaces it; mod code continues.
    public static void MessageBox(string message) => Global("MessageBox", message);

    public static string PlatformName => Global("GetPlatformName").AsString();
    public static float VersionNumber => Global("GetVersionNumber").AsFloat();

    private static Value Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Debug", function, args);
}

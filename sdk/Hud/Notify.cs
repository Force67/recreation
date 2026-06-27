using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Net;

// The severity of a toast, so the HUD can colour it without parsing the text.
public enum NoticeKind
{
    Info,
    Success,
    Warning,
    Error,
}

// One transient toast: the message, its kind, and how long it has left before it
// fades.
public sealed class Notification
{
    public string Text { get; }
    public NoticeKind Kind { get; }

    // Seconds left before this notice expires. HudKit's frame hook decrements it.
    public float Remaining { get; internal set; }

    internal Notification(string text, NoticeKind kind, float seconds)
    {
        Text = text;
        Kind = kind;
        Remaining = seconds;
    }
}

// Transient corner toasts. Each renders on the HUD and is also recorded in an
// in-memory list with its lifetime, so a headless server can observe what it told
// players and tests can drive expiry by advancing frame time. Ticked by HudKit.
public static class Notify
{
    private static readonly List<Notification> Notices = new();

    // The toasts on screen right now, newest last. Read-only; mutate through Show.
    public static IReadOnlyList<Notification> Active => Notices;

    // Show a toast for `seconds`.
    public static void Show(string text, NoticeKind kind = NoticeKind.Info, float seconds = 4f)
    {
        Notices.Add(new Notification(text, kind, seconds));
        Render("Notify", text, (int)kind, seconds);
    }

    public static void Clear() => Notices.Clear();

    // Age every notice by the frame delta and drop the ones that have run out.
    // Iterates backwards so removals do not skip entries.
    internal static void Tick(float deltaTime)
    {
        for (int i = Notices.Count - 1; i >= 0; i--)
        {
            Notices[i].Remaining -= deltaTime;
            if (Notices[i].Remaining <= 0f) Notices.RemoveAt(i);
        }
    }

    private static void Render(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Hud", function, args);
}

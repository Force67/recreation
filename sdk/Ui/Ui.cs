using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using Recreation.Interop;

namespace Recreation;

// Marks a static method as a ultragui handler. ultragui calls a handler named
// "on_<widget>" when that widget is clicked or changes; tag a method with this
// (optionally naming it) to receive the call. The method takes the firing Widget
// or nothing:  [UiHandler] static void on_btn_play(Widget w) { ... }
[AttributeUsage(AttributeTargets.Method)]
public sealed class UiHandlerAttribute : Attribute
{
    public UiHandlerAttribute(string? name = null) => Name = name;
    public string? Name { get; }
}

// The managed face of recreation's ultragui scripting backend. ultragui drives
// the UI; when a handler fires, the native backend dispatches here, and handlers
// reach back through the bound WidgetOps to manipulate widgets. C# replaces Lua
// as ultragui's interaction language -- this is the seam that makes that real.
//
// Bound once at boot from the handshake. With no UI backend (dedicated server)
// it simply stays unbound and every call is an inert no-op.
public static unsafe class Ui
{
    private static IntPtr _ops;  // WidgetOps* handed over at boot (process-lived)
    private static readonly Dictionary<string, Action<Widget>> Handlers = new();
    private static readonly List<Timer> Timers = new();

    private struct Timer { public double Remaining; public Action Callback; }

    internal static WidgetOps* Ops => (WidgetOps*)_ops;
    internal static bool HasOps => _ops != IntPtr.Zero;

    /// True once the UI backend is wired (a windowed client with ultragui).
    public static bool Available => HasOps;

    // Wire the native widget-operation table and discover [UiHandler] methods in
    // the assemblies loaded so far. Called by the script host entrypoint.
    internal static void Bind(WidgetOps* ops)
    {
        _ops = (IntPtr)ops;
        Handlers.Clear();
        Timers.Clear();
        foreach (Assembly asm in AppDomain.CurrentDomain.GetAssemblies())
            ScanAssembly(asm);
    }

    // Discover [UiHandler] static methods in an assembly. Mods call this (or it
    // runs at bind) so their handlers register. Safe to call repeatedly.
    public static void ScanAssembly(Assembly asm)
    {
        Type[] types;
        try { types = asm.GetTypes(); }
        catch (ReflectionTypeLoadException e) { types = Array.FindAll(e.Types, t => t != null)!; }

        foreach (Type type in types)
        {
            foreach (MethodInfo m in type.GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic))
            {
                var attr = m.GetCustomAttribute<UiHandlerAttribute>();
                if (attr == null) continue;
                Action<Widget>? handler = Adapt(m);
                if (handler != null) On(attr.Name ?? m.Name, handler);
            }
        }
    }

    // Build a Widget-taking delegate from a [UiHandler] method that takes either
    // a Widget or no parameters. Returns null for an unsupported signature.
    private static Action<Widget>? Adapt(MethodInfo m)
    {
        ParameterInfo[] ps = m.GetParameters();
        if (ps.Length == 0)
        {
            var d = (Action)Delegate.CreateDelegate(typeof(Action), m);
            return _ => d();
        }
        if (ps.Length == 1 && ps[0].ParameterType == typeof(Widget))
            return (Action<Widget>)Delegate.CreateDelegate(typeof(Action<Widget>), m);
        Console.WriteLine($"[managed/ui] ignoring handler {m.DeclaringType?.Name}.{m.Name}: unsupported signature");
        return null;
    }

    /// Register (or replace) a handler by name programmatically.
    public static void On(string name, Action<Widget> handler) => Handlers[name] = handler;

    /// Remove a handler.
    public static void Off(string name) => Handlers.Remove(name);

    /// Resolve a registered widget name to a Widget (null handle if unknown).
    public static Widget Find(string name)
    {
        if (!HasOps || string.IsNullOrEmpty(name)) return new Widget(0);
        IntPtr s = Marshal.StringToCoTaskMemUTF8(name);
        try { return new Widget(Ops->Find((byte*)s)); }
        finally { Marshal.FreeCoTaskMem(s); }
    }

    /// Run a callback after `seconds` of UI time (ticked from the frame loop).
    public static void After(double seconds, Action callback)
    {
        if (callback != null) Timers.Add(new Timer { Remaining = seconds, Callback = callback });
    }

    // Called by the native backend (via the host) when a ultragui handler fires.
    // Returns 1 if a managed handler claimed it.
    internal static int Dispatch(string funcName, ulong widget)
    {
        if (!Handlers.TryGetValue(funcName, out Action<Widget>? handler)) return 0;
        try { handler(new Widget(widget)); }
        catch (Exception e) { Console.WriteLine($"[managed/ui] handler '{funcName}' threw: {e}"); }
        return 1;
    }

    // Advance pending timers; driven from the script host's per-frame tick.
    internal static void Tick(float dt)
    {
        if (Timers.Count == 0) return;
        for (int i = Timers.Count - 1; i >= 0; i--)
        {
            Timer t = Timers[i];
            t.Remaining -= dt;
            if (t.Remaining > 0) { Timers[i] = t; continue; }
            Timers.RemoveAt(i);
            try { t.Callback(); }
            catch (Exception e) { Console.WriteLine($"[managed/ui] timer threw: {e}"); }
        }
    }
}

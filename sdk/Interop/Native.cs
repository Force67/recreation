using System;

namespace Recreation.Interop;

// The static gateway every engine wrapper calls through. It forwards to the
// bound IEngineBackend: the native bridge in production, or a test fake. Keeping
// it static gives the ergonomic, Unity-like surface (Game.Player with no
// dependency injection) while the backend seam keeps the SDK testable.
public static unsafe class Native
{
    // The active backend. Production binds a NativeBackend over the bridge; tests
    // assign a fake. Null until bound.
    public static IEngineBackend? Backend { get; set; }

    public static bool IsBound => Backend != null;

    // Installs the production backend over the native bridge. Called once by the
    // managed host entrypoint.
    internal static void Bind(ScriptBridge* bridge) => Backend = new NativeBackend(bridge);

    private static IEngineBackend Active =>
        Backend ?? throw new InvalidOperationException("Native backend is not bound");

    public static bool IsScriptLoaded(string type) => Active.IsScriptLoaded(type);
    public static bool LoadScript(string type) => Active.LoadScript(type);
    public static ulong CreateInstance(string type) => Active.CreateInstance(type);
    public static string TypeOf(ulong handle) => Active.TypeOf(handle);

    public static Value CallGlobal(string type, string function, ReadOnlySpan<Value> args) =>
        Active.CallGlobal(type, function, args);

    public static Value CallMethod(ulong self, string function, ReadOnlySpan<Value> args) =>
        Active.CallMethod(self, function, args);

    public static Value GetProperty(ulong self, string name) => Active.GetProperty(self, name);
    public static void SetProperty(ulong self, string name, Value value) =>
        Active.SetProperty(self, name, value);

    public static void Tick(float deltaTime) => Active.Tick(deltaTime);
}

using Recreation.Interop;

namespace Recreation;

// Polling access to the engine's per-frame input snapshot, the counterpoint to
// the KeyPressed event's edges. The snapshot is pushed by the host each frame,
// so this reads sustained button state (holding W, Space, ...) for things the
// events cannot express, like a throttle held open.
public static class Input
{
    // True while `key` is physically down this frame. False when the key is not
    // one the engine delivers. Underpins the Vehicle.Drive throttle/brake/boost
    // mapping; like every input read it returns false with no window or while
    // the debug console is capturing the keyboard.
    public static bool Held(Key key) => Global("Held", (int)key).AsBool();

    private static Value Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Input", function, args);
}
namespace Recreation;

// The keys the engine reports. Mirrors core/input.h Key exactly (same order, so
// the numeric codes match across the boundary). These are the bound keys the
// backend translates; other keys are not delivered. Mods bind handlers to them
// through Hotkeys or by subscribing to KeyPressed, and poll held state through
// Input.Held. Append-only: new codes go on the end, mirroring the engine enum.
public enum Key
{
    W,
    A,
    S,
    D,
    Q,
    E,
    F,
    T,
    C,
    R,
    G,
    X,
    Z,
    B,
    V,
    Space,
    LeftShift,
    LeftCtrl,
    Escape,
    F1,
    F2,
    F3,
    F4,
    F5,
    Delete,
    Backspace,
    Return,
    Num1,
    Num2,
    Num3,
    Num4,
    J,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Tab,
    M,
    Num5,
    Num6,
    L,
}

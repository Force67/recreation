namespace Recreation;

// The keys the engine reports. Mirrors core/input.h Key exactly (same order, so
// the numeric codes match across the boundary). These are the bound keys the
// backend translates; other keys are not delivered. Mods bind handlers to them
// through Hotkeys or by subscribing to KeyPressed.
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
    Space,
    LeftShift,
    LeftCtrl,
    Escape,
    F1,
    F2,
    F3,
    Num1,
    Num2,
    Num3,
    Num4,
    J,
}

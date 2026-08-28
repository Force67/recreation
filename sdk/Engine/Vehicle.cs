using Recreation.Interop;

namespace Recreation;

// The ridden cart (the cart racing kit). A tiny, script-facing surface over the
// host's carriage ride: it lets a ruleset drive the cart the player is sitting
// on and read how fast it is going, without the C# side simulating physics. All
// other game feel stays in the ruleset. Every call is a neutral no-op when
// nothing is being ridden or the runtime kit is absent.
public static class Vehicle
{
    // Drives the ridden cart: `steer` in [-1,1] swings the horse's heading,
    // `throttle` in [0,1] scales its pace (0 holds it). Called each frame while
    // racing; the override clears when the player gets off. No-op if nothing is
    // being ridden.
    public static void Drive(float steer, float throttle) =>
        Global("Drive", steer, throttle);

    // The ridden cart's forward speed in m/s (0 when nothing is being ridden).
    public static float Speed => Global("Speed").AsFloat();

    // True while the player is riding a cart (from the per-frame snapshot), the
    // signal a ruleset uses to know a race can run.
    public static bool Riding => Global("Riding").AsBool();

    // Snaps the ridden cart back to a game-space position (x, up y, z), so a
    // ruleset can respawn to the last checkpoint. The cart, not the player, is
    // what moves, because the player is bolted to the seat. No-op when nothing
    // is being ridden.
    public static void MoveTo(float x, float y, float z) =>
        Global("MoveTo", x, y, z);

    private static Value Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Vehicle", function, args);
}
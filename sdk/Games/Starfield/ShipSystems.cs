using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised after a successful grav jump, carrying the distance covered and the fuel
// left.
public readonly struct GravJumped(float distanceLy, float fuelRemaining) : IGameEvent
{
    public float DistanceLy { get; } = distanceLy;
    public float FuelRemaining { get; } = fuelRemaining;
}

// Raised when the ship's cargo hold crosses into or out of being over capacity.
public readonly struct CargoOverCapacityChanged(bool over, float mass, float capacity) : IGameEvent
{
    public bool Over { get; } = over;
    public float Mass { get; } = mass;
    public float Capacity { get; } = capacity;
}

// The ship layer of Starfield: grav-drive fuel and jump range, plus the cargo hold's
// mass limit. The engine has no ship-state stat for a scripted save, so this is
// managed soft logic. Two faithful rules drive it: a grav jump can only cover up to
// the drive's jump range and only if there is fuel for the distance (fuel spent
// scales with light-years), and an over-capacity hold locks the grav drive entirely
// (Starfield grounds an over-mass ship). The hold's mass also gates fast travel,
// joining MassEncumbrance's reasons on the shared FastTravel gate. Driven by a public
// API (Jump / Refuel / Load / Unload) and an optional jump hotkey, never by an
// engine native it cannot reach. Self-contained state; tests reset it.
public sealed class ShipSystems : GameBehaviour
{
    // The grav drive's tank: current and maximum fuel.
    public float MaxFuel { get; set; } = 100f;
    // Fuel burned per light-year jumped.
    public float FuelPerLightYear { get; set; } = 4f;
    // The farthest a single jump can reach, the drive's jump range in light-years.
    public float JumpRangeLy { get; set; } = 18f;
    // The cargo hold's mass capacity.
    public float CargoCapacity { get; set; } = 600f;
    // The key that triggers a max-range jump by hand, null (off) unless bound.
    public Key? JumpKey { get; set; }

    public float Fuel { get; private set; }
    public float CargoMass { get; private set; }

    public bool IsOverCapacity => CargoMass > CargoCapacity;

    // The farthest the ship can jump right now: the lesser of its range and what the
    // fuel in the tank can pay for, and zero while grounded by an over-mass hold.
    public float ReachableLy => IsOverCapacity ? 0f
        : MathF.Min(JumpRangeLy, FuelPerLightYear > 0f ? Fuel / FuelPerLightYear : JumpRangeLy);

    public bool CanJump(float distanceLy) => distanceLy > 0f && distanceLy <= ReachableLy;

    private bool _overCapacity;
    private EventBus.Subscription? _hotkey;

    protected override void OnStart()
    {
        Fuel = MaxFuel;  // a fresh save leaves the dock with a full tank
        if (JumpKey is { } key)
            _hotkey = Hotkeys.Bind(key, () => Jump(ReachableLy));
    }

    protected override void OnDestroy() => _hotkey?.Dispose();

    // Grav-jumps `distanceLy` light-years if the drive can reach it: spends the fuel
    // and announces the jump. Returns whether it fired. A jump beyond range, beyond
    // the fuel, or with an over-mass hold is refused.
    public bool Jump(float distanceLy)
    {
        if (!CanJump(distanceLy)) return false;
        Fuel = MathF.Max(0f, Fuel - distanceLy * FuelPerLightYear);
        EventBus.Publish(new GravJumped(distanceLy, Fuel));
        return true;
    }

    // Tops the tank up by `amount` (a He3 refuel), never past the maximum.
    public void Refuel(float amount)
    {
        if (amount <= 0f) return;
        Fuel = MathF.Min(MaxFuel, Fuel + amount);
    }

    // Loads `mass` of cargo into the hold (no upper clamp: a hold can be overloaded,
    // which grounds the drive until trimmed). Re-evaluates the over-capacity gate.
    public void Load(float mass)
    {
        if (mass <= 0f) return;
        CargoMass += mass;
        UpdateCapacityGate();
    }

    // Unloads up to `mass` of cargo, never below empty.
    public void Unload(float mass)
    {
        if (mass <= 0f) return;
        CargoMass = MathF.Max(0f, CargoMass - mass);
        UpdateCapacityGate();
    }

    // Applies the over-capacity gate once per transition so the fast-travel reason
    // and the event never stack or leak.
    private void UpdateCapacityGate()
    {
        bool over = IsOverCapacity;
        if (over == _overCapacity) return;
        _overCapacity = over;
        if (over) FastTravel.Block("cargo");
        else FastTravel.Unblock("cargo");
        EventBus.Publish(new CargoOverCapacityChanged(over, CargoMass, CargoCapacity));
    }
}

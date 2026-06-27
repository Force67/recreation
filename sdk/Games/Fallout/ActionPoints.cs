using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Raised when the Action Point pool changes by a spend or a refill.
public readonly struct ActionPointsChanged(float current, float max) : IGameEvent
{
    public float Current { get; } = current;
    public float Max { get; } = max;
}

// Fallout 4's VATS Action Points as managed soft logic, a GameBehaviour. AP is a
// pool that drains when the player spends it (a VATS shot, a sprint) and regenerates
// over time while idle, with a short pause after a spend before it refills, the way
// the game gates AP regen briefly after VATS. The engine has no AP stat, so this is
// the authority: Spend is the public API a VATS hook or the debug panel calls, and
// the regen runs on the update loop. Agility sets the maximum, so it reads through
// Special, tying the SPECIAL attribute to a real consequence.
public sealed class ActionPoints : GameBehaviour
{
    // AP restored per real second once regen resumes.
    public float RegenPerSecond { get; set; } = 30f;
    // Seconds after a spend before AP begins refilling again.
    public float RegenDelay { get; set; } = 1.0f;

    // The base pool and the bonus per Agility point above 1, mirroring Fallout 4's
    // "AGI raises AP" rule.
    public float BaseMax { get; set; } = 60f;
    public float MaxPerAgility { get; set; } = 10f;

    public float Max => BaseMax + (Special.Rank(Game.Player, SpecialAttribute.Agility) - 1) * MaxPerAgility;
    public float Current { get; private set; } = float.NaN;  // NaN until first synced to Max

    private float _regenPauseRemaining;

    protected override void OnStart() => Current = Max;

    protected override void OnUpdate(float deltaTime)
    {
        if (deltaTime <= 0f) return;

        // Persistent VATS action-point bar (amber), always shown like Fallout's.
        Hud.Gauge("ap", Max > 0f ? Current / Max : 0f, "Action Points", 0xf0d24affu);

        if (_regenPauseRemaining > 0f)
        {
            _regenPauseRemaining = Math.Max(0f, _regenPauseRemaining - deltaTime);
            return;
        }
        if (Current < Max) Set(Current + RegenPerSecond * deltaTime);
    }

    // True when the pool can cover `cost`.
    public bool CanSpend(float cost) => cost >= 0f && Current >= cost;

    // Spends `cost` AP for an action (a VATS shot, a sprint tick). Returns false
    // when the pool is too low, leaving it untouched. A successful spend pauses
    // regeneration for RegenDelay, matching the post-VATS pause.
    public bool Spend(float cost)
    {
        if (!CanSpend(cost)) return false;
        _regenPauseRemaining = RegenDelay;
        Set(Current - cost);
        return true;
    }

    // Tops the pool back to full (a rest, or a Stimpak-style instant refill).
    public void Refill() => Set(Max);

    private void Set(float value)
    {
        float clamped = Math.Clamp(value, 0f, Max);
        if (clamped == Current) return;
        Current = clamped;
        EventBus.Publish(new ActionPointsChanged(Current, Max));
    }
}

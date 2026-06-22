using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Slowly restores the Sole Survivor's Health out of combat, the Fallout 4 take on
// AttributeRegeneration. Fallout has no Magicka/Stamina pools, so this regenerates
// Health alone: a fraction of base per second, paused while fighting. Like its
// Skyrim cousin it is pure managed soft logic, tunable and replaceable by a mod.
public sealed class CommonwealthRegeneration : GameBehaviour
{
    // Fraction of base Health restored per second out of combat.
    public float HealthRatePerSecond { get; set; } = 0.012f;

    // While true, Health does not recover during combat.
    public bool PauseHealthInCombat { get; set; } = true;

    protected override void OnUpdate(float deltaTime)
    {
        Actor player = Game.Player;
        if (!player.Exists) return;
        if (PauseHealthInCombat && player.IsInCombat) return;
        if (HealthRatePerSecond <= 0f) return;

        float current = player.GetValue(ActorValue.Health);
        float baseValue = player.GetBaseValue(ActorValue.Health);
        if (current >= baseValue) return;  // RestoreValue clamps, but skip needless traffic
        player.RestoreValue(ActorValue.Health, baseValue * HealthRatePerSecond * deltaTime);
    }
}

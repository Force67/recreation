using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Regenerates the player's primary attributes over time, the classic Skyrim
// mechanic, implemented entirely in managed code as soft gameplay logic. Each
// frame it restores Health, Magicka and Stamina toward their base by a fraction
// of that base, so a bigger pool refills faster in absolute terms but at the
// same pace proportionally.
//
// This is a worked example of the kind of system that belongs in the scripting
// runtime rather than the engine: tunable, replaceable, and reachable by other
// mods. Rates are public so a mod can retune them, and the combat pause is the
// usual rule that health does not recover while fighting.
public sealed class AttributeRegeneration : GameBehaviour
{
    // Fraction of base value restored per second. Defaults approximate Skyrim's
    // out-of-combat feel; tune freely.
    public float HealthRatePerSecond { get; set; } = 0.007f;
    public float MagickaRatePerSecond { get; set; } = 0.03f;
    public float StaminaRatePerSecond { get; set; } = 0.05f;

    // While true, health does not regenerate during combat (magicka and stamina
    // still do).
    public bool PauseHealthInCombat { get; set; } = true;

    protected override void OnUpdate(float deltaTime)
    {
        Actor player = Game.Player;
        if (!player.Exists) return;

        bool inCombat = player.IsInCombat;
        if (!(PauseHealthInCombat && inCombat))
            Regenerate(player, ActorValue.Health, HealthRatePerSecond, deltaTime);
        Regenerate(player, ActorValue.Magicka, MagickaRatePerSecond, deltaTime);
        Regenerate(player, ActorValue.Stamina, StaminaRatePerSecond, deltaTime);
    }

    // Restores a fraction of base toward the cap. RestoreValue already clamps at
    // base, so no overshoot check is needed; skip the call when already full to
    // avoid needless engine traffic.
    private static void Regenerate(Actor actor, string attribute, float ratePerSecond, float dt)
    {
        if (ratePerSecond <= 0f) return;
        float current = actor.GetValue(attribute);
        float baseValue = actor.GetBaseValue(attribute);
        if (current >= baseValue) return;
        actor.RestoreValue(attribute, baseValue * ratePerSecond * dt);
    }
}

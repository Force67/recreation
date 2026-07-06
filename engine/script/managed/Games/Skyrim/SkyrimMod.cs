using System;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// The built-in Skyrim gameplay layer: the one place the engine's Skyrim-specific
// soft logic is assembled and registered, keeping it isolated from the
// game-agnostic SDK. The mod host discovers this like any other mod and runs
// OnLoad once at boot, where it brings each Skyrim system online.
//
// As more games land, this is gated on the active game profile so it installs
// only under Skyrim; until the engine exposes that, it registers unconditionally
// in this Skyrim-targeted build.
[Mod("Skyrim", Author = "Recreation", Version = "1.0.0")]
public sealed class SkyrimMod : IMod
{
    public void OnLoad()
    {
        Console.WriteLine("[skyrim] installing gameplay systems");

        // Regen rates are tunable from config (Skyrim.json), defaulting to the
        // built-in feel.
        ModConfig config = ModConfig.Load("Skyrim");
        ModHost.Register(new AttributeRegeneration
        {
            HealthRatePerSecond = config.GetFloat("healthRegenPerSecond", 0.007f),
            MagickaRatePerSecond = config.GetFloat("magickaRegenPerSecond", 0.03f),
            StaminaRatePerSecond = config.GetFloat("staminaRegenPerSecond", 0.05f),
        });
        ModHost.Register(new QuestProgressTracker());
        ModHost.Register(new CombatTracker());
        ModHost.Register(new CombatFastTravelLock());
        ModHost.Register(new IndoorFastTravelLock());
        ModHost.Register(new EssentialProtection());
        ModHost.Register(new InjurySlowdown());
        ModHost.Register(new TimeOfDayService());
        ModHost.Register(new Encumbrance());

        // Survival hunger is opt-in (it is not vanilla behaviour); enable it with
        // "survivalNeeds": true in Skyrim.json.
        if (config.GetBool("survivalNeeds", false))
            ModHost.Register(new SurvivalNeeds());
    }
}

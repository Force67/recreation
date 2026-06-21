using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// The built-in Skyrim gameplay layer: the one place the engine's Skyrim-specific
// soft logic is assembled and registered, keeping it isolated from the
// game-agnostic SDK. The mod host discovers this like any other mod and runs
// OnLoad once at boot, where it brings each Skyrim system online.
//
// Gated on the active game so its systems install only when Skyrim is the primary
// world, keeping them out of a Fallout or Starfield session that shares this
// assembly (those games install their own layers, see StarfieldMod).
[Mod("Skyrim", Author = "Recreation", Version = "1.0.0")]
public sealed class SkyrimMod : IMod
{
    // The Skyrim content domain's name (the game profile name the engine reports).
    private const string GameName = "Skyrim Special Edition";

    public void OnLoad()
    {
        if (Domains.Primary?.Name != GameName) return;

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
        ModHost.Register(new LocationDiscovery());
        ModHost.Register(new Harvesting());
        ModHost.Register(new BookLearning());
        ModHost.Register(new RacialAbilities());
        ModHost.Register(new BlessingUpkeep());
        ModHost.Register(new VampireProgression());
        ModHost.Register(new BeastForm());
        ModHost.Register(new ShoutCooldown());

        // Survival hunger is opt-in (it is not vanilla behaviour); enable it with
        // "survivalNeeds": true in Skyrim.json.
        if (config.GetBool("survivalNeeds", false))
            ModHost.Register(new SurvivalNeeds());

        // The meditate-to-rest power binds a key, so it is opt-in too.
        if (config.GetBool("meditationPower", false))
            ModHost.Register(new MeditationPower());
    }
}

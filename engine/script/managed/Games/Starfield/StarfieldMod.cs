using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// The built-in Starfield gameplay layer: the one place the engine's
// Starfield-specific soft logic is assembled and registered, the twin of
// SkyrimMod. The mod host discovers this like any other mod and runs OnLoad once
// at boot.
//
// Unlike SkyrimMod this gates on the active game so its systems install only when
// Starfield is the primary world, keeping the O2/CO2 loop out of a Skyrim or
// Fallout session that shares the same managed assembly.
[Mod("Starfield", Author = "Recreation", Version = "1.0.0")]
public sealed class StarfieldMod : IMod
{
    public void OnLoad()
    {
        if (Domains.Primary?.Name != Starfield.GameName) return;

        Console.WriteLine("[starfield] installing gameplay systems");

        // The survival loop is tunable from config (Starfield.json), defaulting to
        // the built-in feel.
        ModConfig config = ModConfig.Load("Starfield");
        ModHost.Register(new OxygenCo2
        {
            DrainPerSecond = config.GetFloat("oxygenDrainPerSecond", 12f),
            RegenPerSecond = config.GetFloat("oxygenRegenPerSecond", 18f),
            RestorePerSource = config.GetFloat("oxygenRestorePerSource", 60f),
            CarbonDioxideRisePerSecond = config.GetFloat("co2RisePerSecond", 10f),
            CarbonDioxideDecayPerSecond = config.GetFloat("co2DecayPerSecond", 14f),
            OxygenLowAt = config.GetFloat("oxygenLowAt", 35f),
            CarbonDioxideBuildingAt = config.GetFloat("co2BuildingAt", 25f),
            HypoxiaAfterSeconds = config.GetFloat("hypoxiaAfterSeconds", 5f),
        });
        ModHost.Register(new EnvironmentalHazards
        {
            ExposurePerSecond = config.GetFloat("hazardExposurePerSecond", 20f),
            RecoveryPerSecond = config.GetFloat("hazardRecoveryPerSecond", 25f),
        });
        ModHost.Register(new MassEncumbrance
        {
            SpeedPenalty = config.GetFloat("massSpeedPenalty", 35f),
        });
        // Surfaces the systems above to the player as corner notifications.
        ModHost.Register(new StarfieldNotifications());
    }
}

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
        // Sleep recovery: rest tops up the breath meters and grants a rested bonus.
        ModHost.Register(new WellRested
        {
            Bonus = config.GetFloat("wellRestedBonus", 0.1f),
            DurationGameHours = config.GetFloat("wellRestedHours", 24f),
        });
        // Quest progress pays character XP, driving leveling from real play.
        ModHost.Register(new StarfieldQuestRewards
        {
            XpPerStage = config.GetFloat("questXpPerStage", 50f),
        });
        // Kills during the player's fights pay XP too, the combat half of leveling.
        ModHost.Register(new StarfieldCombatRewards
        {
            XpPerKill = config.GetFloat("combatXpPerKill", 15f),
        });
        // Finding a new location pays XP as well, the exploration source.
        ModHost.Register(new StarfieldDiscovery
        {
            XpPerDiscovery = config.GetFloat("discoveryXp", 25f),
        });
        // The ship layer: grav-drive fuel/range and the cargo-hold mass limit. The
        // social/crime systems (FactionReputation, CrewAffinity, Bounties) are static
        // registries driven on demand by quests and deeds, so they stay unregistered.
        ModHost.Register(new ShipSystems
        {
            MaxFuel = config.GetFloat("shipMaxFuel", 100f),
            FuelPerLightYear = config.GetFloat("shipFuelPerLightYear", 4f),
            JumpRangeLy = config.GetFloat("shipJumpRangeLy", 18f),
            CargoCapacity = config.GetFloat("shipCargoCapacity", 600f),
        });
        // The cross-game NPC reaction layer (Modding): a wanted/heat meter, the
        // guard confrontation it drives, NPC daily schedules, and bystander alarm.
        // Shared with the other primaries (see SkyrimMod); NpcDisposition/NpcMorale
        // are static resolvers and need no registration.
        var wanted = new WantedLevel();
        ModHost.Register(wanted);
        ModHost.Register(new GuardResponse(wanted));
        ModHost.Register(new NpcRoutine());
        ModHost.Register(new BystanderReaction());

        // Surfaces the systems above to the player as corner notifications.
        ModHost.Register(new StarfieldNotifications());

        // A keypad panel to exercise the gameplay by hand; off unless asked for.
        if (config.GetBool("debug", false))
            ModHost.Register(new StarfieldGameplayDebug());
    }
}

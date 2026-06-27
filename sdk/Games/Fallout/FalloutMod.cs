using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// The built-in Fallout 4 gameplay layer, the twin of SkyrimMod and StarfieldMod:
// the one place the engine's Fallout-specific soft logic is assembled and
// registered. The mod host discovers it like any other mod and runs OnLoad once
// at boot, where it brings each Commonwealth system online.
//
// Gated on the active game so its systems install only when Fallout 4 is the
// primary world, keeping them out of a Skyrim or Starfield session that shares
// this assembly (those games install their own layers).
[Mod("Fallout 4", Author = "Recreation", Version = "1.0.0")]
public sealed class FalloutMod : IMod
{
    public void OnLoad()
    {
        if (Domains.Primary?.Name != Fallout.GameName) return;

        Console.WriteLine("[fallout] installing gameplay systems");

        // Tunable from config (Fallout.json), defaulting to the built-in feel.
        ModConfig config = ModConfig.Load("Fallout");
        ModHost.Register(new CommonwealthRegeneration
        {
            HealthRatePerSecond = config.GetFloat("healthRegenPerSecond", 0.012f),
            PauseHealthInCombat = config.GetBool("pauseHealthInCombat", true),
        });
        // Quest progress surfaces through the Pip-Boy as journal notifications.
        ModHost.Register(new PipBoyQuestLog());
        // Radiation eats max Health out in the hot zones; RadAway and sleep cure it.
        ModHost.Register(new Radiation
        {
            RadsPerSecond = config.GetFloat("radsPerSecond", 8f),
            CurePerDose = config.GetFloat("radAwayCure", 300f),
            MinorAt = config.GetFloat("radMinorAt", 200f),
            AdvancedAt = config.GetFloat("radAdvancedAt", 500f),
            CriticalAt = config.GetFloat("radCriticalAt", 800f),
            MaxRads = config.GetFloat("radMax", 1000f),
        });
        ModHost.Register(new Encumbrance
        {
            SpeedPenalty = config.GetFloat("encumbranceSpeedPenalty", 40f),
        });
        // Quest, kills and survival adrenaline drive XP -> level -> perk points.
        ModHost.Register(new FalloutQuestRewards
        {
            XpPerStage = config.GetFloat("questXpPerStage", 50f),
        });
        ModHost.Register(new FalloutCombatRewards
        {
            XpPerKill = config.GetFloat("combatXpPerKill", 15f),
        });
        ModHost.Register(new Adrenaline
        {
            KillsPerRank = config.GetInt("adrenalineKillsPerRank", 5),
            DamagePerRank = config.GetFloat("adrenalineDamagePerRank", 0.05f),
            DecaySeconds = config.GetFloat("adrenalineDecaySeconds", 60f),
        });
        ModHost.Register(new ActionPoints
        {
            RegenPerSecond = config.GetFloat("apRegenPerSecond", 30f),
            RegenDelay = config.GetFloat("apRegenDelay", 1.0f),
            BaseMax = config.GetFloat("apBaseMax", 60f),
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
        ModHost.Register(new CommonwealthNotifications());

        // A keypad panel to exercise the gameplay by hand; off unless asked for.
        if (config.GetBool("debug", false))
            ModHost.Register(new FalloutGameplayDebug());
    }
}

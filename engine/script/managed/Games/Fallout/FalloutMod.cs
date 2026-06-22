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
    }
}

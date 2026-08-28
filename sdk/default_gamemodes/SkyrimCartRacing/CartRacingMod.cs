using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.SkyrimCartRacing;

// Super Cart Xtreme Racing, an optional default gamemode: turns any mounted
// cart ride into a checkpoint race. A course is generated around wherever you
// climb aboard, and the rider drives with W/A/S/D, boosts with Space, and
// respawns with R. The ruleset is pure C# over the cart drive kit
// (Input.Held, Vehicle.Drive/Speed/Riding/MoveTo); the host supplies the feel.
[Mod("SkyrimCartRacing", Author = "Recreation", Version = "1.0.0")]
public sealed class CartRacingMod : IMod
{
    // The Skyrim content domain's name, so the race only arms in Skyrim.
    private const string GameName = "Skyrim Special Edition";

    public void OnLoad()
    {
        if (Domains.Primary?.Name != GameName) return;

        ModConfig config = ModConfig.Load("SkyrimCartRacing");
        ModHost.Register(new CartRacingBehaviour
        {
            Laps = config.GetInt("laps", 3),
            CheckpointCount = config.GetInt("checkpointCount", 6),
            CourseRadius = config.GetFloat("courseRadius", 4000f),
            CheckpointReach = config.GetFloat("checkpointReach", 250f),
            BoostDrainPerSecond = config.GetFloat("boostDrainPerSecond", 0.25f),
            BoostRegenPerSecond = config.GetFloat("boostRegenPerSecond", 0.15f),
            WrongWayRespawnSeconds = config.GetFloat("wrongWayRespawnSeconds", 5f),
        });
    }
}

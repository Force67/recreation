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
        ModHost.Register(new AttributeRegeneration());
        ModHost.Register(new QuestProgressTracker());
        ModHost.Register(new CombatTracker());
    }
}

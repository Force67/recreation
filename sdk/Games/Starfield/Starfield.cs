using Recreation;

namespace Recreation.Games.Starfield;

// The entry point for mods that target Starfield content, the analog of the
// static Game class for the primary world and a twin of Games.Fallout.Fallout.
// recreation can run several games at once; Game/Form/Actor target the primary
// (rendered) game, while a Starfield mod reaches its content here no matter where
// Starfield sits in the load order. A mod can therefore read Skyrim through Game
// and Starfield through Starfield in the same session.
public static class Starfield
{
    public const string GameName = "Starfield";

    // The Starfield content domain, or null when Starfield is not loaded.
    public static GameWorld? World => Domains.Get(GameName);

    // True when Starfield content is live this session.
    public static bool Loaded => World != null;

    // Resolves a Starfield form by its runtime (load-order) form id, or
    // WorldForm.None when Starfield is not loaded.
    public static WorldForm GetForm(uint formId) =>
        World is { } world ? world.GetForm(formId) : WorldForm.None;

    // The display name of the Starfield form at formId, empty when unknown or
    // Starfield is not loaded. The text resolves from Starfield's own strings.
    public static string NameOf(uint formId) => GetForm(formId).Name;
}

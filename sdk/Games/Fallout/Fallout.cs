using Recreation;

namespace Recreation.Games.Fallout;

// The entry point for mods that target Fallout 4 content, the analog of the
// static Game class for the primary world. recreation can run several games at
// once; Game/Form/Actor target the primary (rendered) game, while a Fallout mod
// reaches its content here no matter where Fallout 4 sits in the load order. A
// mod can therefore read Skyrim through Game and Fallout through Fallout in the
// same session.
public static class Fallout
{
    public const string GameName = "Fallout 4";

    // The Fallout 4 content domain, or null when Fallout 4 is not loaded.
    public static GameWorld? World => Domains.Get(GameName);

    // True when Fallout 4 content is live this session.
    public static bool Loaded => World != null;

    // Resolves a Fallout 4 form by its runtime (load-order) form id, or
    // WorldForm.None when Fallout 4 is not loaded.
    public static WorldForm GetForm(uint formId) =>
        World is { } world ? world.GetForm(formId) : WorldForm.None;

    // The display name of the Fallout 4 form at formId, empty when unknown or
    // Fallout 4 is not loaded. The text resolves from Fallout's own strings.
    public static string NameOf(uint formId) => GetForm(formId).Name;
}

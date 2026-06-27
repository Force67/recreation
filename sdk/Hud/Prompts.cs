using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;

namespace Recreation.Net;

// One contextual prompt: a stable id (so a mod targets the same prompt every
// frame), the line to show, the key hint, and whether it is currently on screen.
public sealed class Prompt
{
    public string Id { get; }
    public string Label { get; internal set; }
    public string Key { get; internal set; }
    public bool Visible { get; internal set; }

    internal Prompt(string id, string label, string key)
    {
        Id = id;
        Label = label;
        Key = key;
        Visible = true;
    }
}

// Contextual interaction prompts (the "Press E to open" hint). Keyed by id, so Show
// is idempotent: re-showing updates the prompt in place rather than piling up
// duplicates.
public static class Prompts
{
    private static readonly Dictionary<string, Prompt> Registry = new();

    // Every prompt on screen right now. Materialised so callers can index or count.
    public static IReadOnlyList<Prompt> Active =>
        Registry.Values.Where(p => p.Visible).ToList();

    // Create or update the prompt with this id, mark it visible, and render it.
    public static void Show(string id, string label, string key = "E")
    {
        if (Registry.TryGetValue(id, out Prompt? prompt))
        {
            prompt.Label = label;
            prompt.Key = key;
            prompt.Visible = true;
        }
        else
        {
            Registry[id] = new Prompt(id, label, key);
        }
        Render("Prompt", id, label, key);
    }

    // Take the prompt off screen. The entry is kept (just marked hidden) so a later
    // Show reuses the same object.
    public static void Hide(string id)
    {
        if (Registry.TryGetValue(id, out Prompt? prompt)) prompt.Visible = false;
        Render("ClearPrompt", id);
    }

    public static Prompt? Get(string id) => Registry.TryGetValue(id, out Prompt? p) ? p : null;

    public static void Clear() => Registry.Clear();

    private static void Render(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Hud", function, args);
}

using System.Collections.Generic;
using System.Linq;

namespace Recreation.Games.Skyrim;

// The player's known spells: a managed mirror of their spell list, since the
// engine has no "knows spell" query. Learning adds the spell to the player and
// records it here; spell tomes route through this, and a mod can teach or query
// directly. Persistent player knowledge, so it is not tied to the mod-host
// lifecycle; tests clear it explicitly.
public static class Spellbook
{
    private static readonly HashSet<ulong> Known = new();

    // Teaches `spell` to the player and records it; returns true if it was new.
    public static bool Learn(Spell spell)
    {
        if (!spell.Exists) return false;
        Game.Player.AddSpell(spell);
        return Known.Add(spell.Handle);
    }

    public static bool Knows(Form spell) => Known.Contains(spell.Handle);

    public static int Count => Known.Count;

    public static IEnumerable<Spell> All => Known.Select(Spell.From);

    public static void Clear() => Known.Clear();
}

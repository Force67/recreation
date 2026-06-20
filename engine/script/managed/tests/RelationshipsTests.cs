using Recreation;
using Recreation.Games.Skyrim;

namespace Recreation.Tests;

// Covers the relationship graph: ranks are mutual, default to neutral, drive the
// friendly/hostile checks and a 0..100 disposition.
public static class RelationshipsTests
{
    public static void Run(Check check)
    {
        Relationships.Clear();

        var player = Actor.From(0x14);
        var lydia = Actor.From(0x100);
        var bandit = Actor.From(0x200);
        var stranger = Actor.From(0x300);

        // Unset pairs are neutral acquaintances.
        check.Equal("unset is acquaintance", RelationshipRank.Acquaintance,
                    Relationships.Get(player, stranger));
        check.Equal("neutral disposition is 50", 50, Relationships.Disposition(player, stranger));

        // A set rank reads back the same in either order (mutual).
        Relationships.Set(player, lydia, RelationshipRank.Ally);
        check.Equal("rank set one way", RelationshipRank.Ally, Relationships.Get(player, lydia));
        check.Equal("reads the same the other way", RelationshipRank.Ally,
                    Relationships.Get(lydia, player));

        // Friendly / hostile checks follow the rank.
        check.That("an ally is friendly", Relationships.AreFriendly(player, lydia));
        check.That("an ally is not hostile", !Relationships.AreHostile(player, lydia));

        Relationships.Set(player, bandit, RelationshipRank.Enemy);
        check.That("an enemy is hostile", Relationships.AreHostile(player, bandit));
        check.That("an enemy is not friendly", !Relationships.AreFriendly(player, bandit));

        // Disposition tracks the rank on the 0..100 scale.
        check.Equal("ally disposition", 86, Relationships.Disposition(player, lydia));   // 50 + 3*12
        check.Equal("enemy disposition", 14, Relationships.Disposition(player, bandit)); // 50 - 3*12

        // A rank can be raised, and a Lover clamps near the top.
        Relationships.Set(player, lydia, RelationshipRank.Lover);
        check.Equal("rank updated", RelationshipRank.Lover, Relationships.Get(player, lydia));
        check.Equal("lover disposition clamps to 0..100", 98, Relationships.Disposition(player, lydia));

        check.Equal("two relationships tracked", 2, Relationships.Count);
        Relationships.Clear();
        check.Equal("clear empties the graph", 0, Relationships.Count);
    }
}

using System.Collections.Generic;
using System.Linq;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// A race's innate traits applied to an actor: the constant-effect abilities (frost
// resistance, water-breathing, fortify-this-or-that) become permanent modifiers,
// and the once-a-day racial power is surfaced for a mod to bind. The engine does
// not apply racial abilities on its own, so this is the soft logic that makes a
// Nord actually resist frost. Composes Race.Abilities, Spell.Type and the
// Abilities scheduler; the player usually gets these once at game start.
public static class RaceTraits
{
    // Grants the actor its race's passive abilities (the constant-effect ones),
    // fortifying each value the abilities touch. Idempotent, since Abilities.Apply
    // is. Returns how many abilities were applied.
    public static int Grant(Actor actor)
    {
        int applied = 0;
        foreach (Spell ability in actor.Race.Abilities.Where(s => s.Type == SpellType.Ability))
            if (Abilities.Apply(actor, ability)) applied++;
        return applied;
    }

    // Revokes the race's passive abilities, reverting their modifiers.
    public static void Revoke(Actor actor)
    {
        foreach (Spell ability in actor.Race.Abilities.Where(s => s.Type == SpellType.Ability))
            Abilities.Remove(actor, ability);
    }

    // The race's powers (the once-a-day greater powers), for a mod to bind to a
    // key and fire through Powers.Use.
    public static IEnumerable<Spell> Powers(Actor actor) =>
        actor.Race.Abilities.Where(s => s.Type == SpellType.Power);

    // Raises each of the actor's skills by its racial starting bonus. A one-time
    // character-creation step: calling it again double-applies, so a mod grants it
    // once when the character is made.
    public static void GrantSkillBonuses(Actor actor)
    {
        foreach (RaceSkillBonus bonus in actor.Race.SkillBonuses)
            actor.ModValue(bonus.Skill, bonus.Bonus);
    }
}

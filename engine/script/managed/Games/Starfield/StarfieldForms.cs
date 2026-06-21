namespace Recreation.Games.Starfield;

// Well-known Starfield.esm form ids the gameplay layer refers to by name instead
// of a bare hex literal. These are base-game ids (load order 00); resolve them at
// runtime with Starfield.GetForm.
//
// PLACEHOLDER ids: unlike SkyrimForms these were not probed from the .esm, so the
// hex values are stand-ins chosen only to be distinct. Replace them with the real
// records once dumped; the systems reference the names, so a fix is one edit here.
public static class StarfieldForms
{
    // The keyword marking an item as breathable air the player can top off from
    // (an O2 canister or aid item). OxygenCo2 refills oxygen when one is consumed,
    // the way SurvivalNeeds feeds on the food keyword. PLACEHOLDER.
    public const uint OxygenSourceKeyword = 0x0000A101;
}

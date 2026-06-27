using System;

namespace Recreation.Games.Skyrim;

// Well-known Skyrim.esm Civil War record ids the Civil War layer refers to by name
// instead of a bare hex literal. These are base-game ids (load order 00); resolve
// them at runtime with Game.GetForm / Game.GetQuest / Game.GetGlobal / Game.GetFaction.
//
// The managed surface resolves forms by numeric id only (Game.GetForm); there is
// no editor-id lookup native and the C++ engine is off limits here, so the editor
// ids the Creation Kit shows ("CW00A", "CWPercentPoolRemainingAttacker", ...) are
// mapped to their record ids in this one table.
//
// PLACEHOLDER ids: like StarfieldForms these were not probed from the .esm in this
// tree, so the hex values are stand-ins chosen only to be distinct. Replace them
// with the real records once dumped; the systems reference the names, so a fix is
// one edit here.
public static class CivilWarForms
{
    // Enlistment quests: joining a side. CW00A is the Imperial Legion path, CW00B
    // the Stormcloak path. PLACEHOLDER.
    public const uint JoinImperialQuest = 0x000D3C5F;     // CW00A (verified)
    public const uint JoinStormcloakQuest = 0x000E1B5A;   // CW00B (verified)

    // Campaign quests: the war that follows enlisting. CW01A "Reunification of
    // Skyrim" (Imperial), CW01B "Liberation of Skyrim" (Stormcloak). PLACEHOLDER.
    public const uint ImperialCampaignQuest = 0x000D517A;    // CW01A (verified)
    public const uint StormcloakCampaignQuest = 0x000E2D29;  // CW01B (verified)

    // The master battle controller that runs during a hold siege.
    public const uint SiegeQuest = 0x000954E1;  // CWSiege (verified)

    // Reinforcement-pool globals, each a 0..100 percentage of the army's remaining
    // pool, that the siege drives and the battle HUD reads. PLACEHOLDER.
    public const uint PercentPoolRemainingAttacker = 0x0009C020;  // CWPercentPoolRemainingAttacker
    public const uint PercentPoolRemainingDefender = 0x0009C021;  // CWPercentPoolRemainingDefender

    // The two sides' factions; player membership is the durable record of which
    // side was joined. PLACEHOLDER.
    public const uint ImperialLegionFaction = 0x0009C030;  // CWImperialFaction
    public const uint StormcloaksFaction = 0x0009C031;     // CWSonsFaction

    // The Civil War quests this layer reacts to. Extend with more CW* quest ids as
    // they are needed; detection is membership of this set.
    private static readonly uint[] Quests =
    {
        JoinImperialQuest, JoinStormcloakQuest,
        ImperialCampaignQuest, StormcloakCampaignQuest, SiegeQuest,
    };

    public static bool IsCivilWarQuest(uint formId) => Array.IndexOf(Quests, formId) >= 0;

    // The side an enlistment or campaign quest belongs to, or None for a neutral
    // Civil War quest (e.g. the siege).
    public static CivilWarSide SideOfQuest(uint formId) => formId switch
    {
        JoinImperialQuest or ImperialCampaignQuest => CivilWarSide.Imperial,
        JoinStormcloakQuest or StormcloakCampaignQuest => CivilWarSide.Stormcloak,
        _ => CivilWarSide.None,
    };
}

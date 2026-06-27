using System;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Which side of the Civil War the player has thrown in with.
public enum CivilWarSide
{
    None,
    Imperial,
    Stormcloak,
}

// The player's Civil War allegiance, a small static service other systems read
// (the journal toast, the battle HUD, future war-map UI). Allegiance is derived,
// not owned: the tracker behaviour below feeds it from the questline, and it is
// fixed once chosen (you cannot defect mid-war), so reading it is a plain property.
public static class CivilWarState
{
    public static CivilWarSide CurrentSide { get; private set; }

    public static bool HasJoined => CurrentSide != CivilWarSide.None;

    // Raised once, when the player first commits to a side.
    public static event Action<CivilWarSide>? AllegianceChanged;

    // Records the chosen side. Idempotent and one-way: re-setting the same side or
    // clearing back to None does nothing, matching that the war has no defection.
    internal static void Set(CivilWarSide side)
    {
        if (side == CivilWarSide.None || side == CurrentSide) return;
        CurrentSide = side;
        AllegianceChanged?.Invoke(side);
    }

    // Clears the allegiance when the managed world tears down, so a reload starts
    // unaligned.
    internal static void Reset() => CurrentSide = CivilWarSide.None;
}

// Derives the player's allegiance from Civil War progression and feeds it into
// CivilWarState. It watches QuestStageChanged for the enlistment and campaign
// quests (any progress in one settles the side) and, on start, reads faction
// membership so a save where the player already enlisted is recognised at once.
public sealed class CivilWarAllegianceTracker : GameBehaviour
{
    private EventBus.Subscription? _subscription;

    protected override void OnStart()
    {
        _subscription = EventBus.Subscribe<QuestStageChanged>(OnStageChanged);
        DeriveFromFactions();
    }

    protected override void OnDestroy()
    {
        _subscription?.Dispose();
        CivilWarState.Reset();
    }

    private void OnStageChanged(QuestStageChanged e)
    {
        if (e.Stage <= 0) return;  // the quest's set-up stage commits to nothing
        CivilWarState.Set(CivilWarForms.SideOfQuest(e.Quest.FormId));
    }

    private static void DeriveFromFactions()
    {
        Actor player = Game.Player;
        if (!player.Exists) return;

        Faction imperial = Game.GetFaction(CivilWarForms.ImperialLegionFaction);
        Faction stormcloaks = Game.GetFaction(CivilWarForms.StormcloaksFaction);
        if (imperial.Exists && player.IsInFaction(imperial))
            CivilWarState.Set(CivilWarSide.Imperial);
        else if (stormcloaks.Exists && player.IsInFaction(stormcloaks))
            CivilWarState.Set(CivilWarSide.Stormcloak);
    }
}

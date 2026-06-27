namespace Recreation;

// How one faction regards another in combat (the FACT record's XNAM reaction).
public enum CombatReaction
{
    Neutral = 0,
    Enemy = 1,
    Ally = 2,
    Friend = 3,
}

// A faction (FACT record): a group an actor can belong to, with a crime-gold
// ledger and pairwise reactions to other factions. Actor membership lives on
// Actor (GetFactionRank, IsInFaction, ...); this is the faction-wide state.
public class Faction : Form
{
    protected Faction(ulong handle) : base(handle) { }

    public static new Faction From(ulong handle) => new(handle);

    // The reaction toward another faction (0 neutral, 1 enemy, 2 ally, 3 friend),
    // from the record unless a runtime SetReaction overrides it.
    public int GetReaction(Faction other) => Call("GetReaction", other).AsInt();
    public void SetReaction(Faction other, int reaction) => Call("SetReaction", other, reaction);

    // The reaction toward another faction as a typed value.
    public CombatReaction ReactionToward(Faction other) => (CombatReaction)GetReaction(other);

    // The faction's DATA flags (FACT record).
    public int Flags => Call("GetFactionFlags").AsInt();

    // True if this faction tracks the player's bounty (the Track Crime flag): a
    // hold's guards, in other words.
    public bool IsCrimeFaction => (Flags & 0x40) != 0;

    public int CrimeGold
    {
        get => Call("GetCrimeGold").AsInt();
        set => Call("SetCrimeGold", value);
    }
    public void ModCrimeGold(int delta) => Call("ModCrimeGold", delta);
}

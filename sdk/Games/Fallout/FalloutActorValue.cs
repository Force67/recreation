namespace Recreation.Games.Fallout;

// The Fallout 4 actor-value ids, as constants so the combat layer reads
// actor.GetValue(FalloutActorValue.DamageResist) instead of a bare string, the
// Fallout twin of the Skyrim ActorValue table. Fallout 4 reuses Skyrim's
// actor-value engine but renames the stats: the SPECIAL attributes replace the
// skills, and damage is split across three resistance channels. Not exhaustive;
// any engine-known id string also works.
public static class FalloutActorValue
{
    public const string Health = "Health";
    public const string ActionPoints = "ActionPoints";

    // SPECIAL: the seven core attributes, each 1..10.
    public const string Strength = "Strength";
    public const string Perception = "Perception";
    public const string Endurance = "Endurance";
    public const string Charisma = "Charisma";
    public const string Intelligence = "Intelligence";
    public const string Agility = "Agility";
    public const string Luck = "Luck";

    // The three resistance channels a hit is met by.
    public const string DamageResist = "DamageResist";
    public const string EnergyResist = "EnergyResist";
    public const string RadResist = "RadResist";
}

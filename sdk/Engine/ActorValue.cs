namespace Recreation;

// The Skyrim actor-value ids, as constants so mod code reads
// actor.GetValue(ActorValue.Health) instead of a bare string. Not exhaustive;
// any engine-known id string also works.
public static class ActorValue
{
    public const string Health = "Health";
    public const string Magicka = "Magicka";
    public const string Stamina = "Stamina";

    public const string OneHanded = "OneHanded";
    public const string TwoHanded = "TwoHanded";
    public const string Marksman = "Marksman";
    public const string Block = "Block";
    public const string Smithing = "Smithing";
    public const string HeavyArmor = "HeavyArmor";
    public const string LightArmor = "LightArmor";
    public const string Pickpocket = "Pickpocket";
    public const string Lockpicking = "Lockpicking";
    public const string Sneak = "Sneak";
    public const string Alchemy = "Alchemy";
    public const string Speechcraft = "Speechcraft";
    public const string Alteration = "Alteration";
    public const string Conjuration = "Conjuration";
    public const string Destruction = "Destruction";
    public const string Illusion = "Illusion";
    public const string Restoration = "Restoration";
    public const string Enchanting = "Enchanting";

    public const string CarryWeight = "CarryWeight";
    public const string SpeedMult = "SpeedMult";
    public const string Aggression = "Aggression";
    public const string Confidence = "Confidence";
    public const string Morality = "Morality";
}

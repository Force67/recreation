namespace Recreation.Games.Skyrim;

// Well-known Skyrim.esm form ids, so mod code refers to gold or the food keyword
// by name instead of a bare hex literal. These are base-game ids (load order 00);
// resolve them at runtime with Game.GetForm.
public static class SkyrimForms
{
    // The gold currency item (Gold001).
    public const uint Gold = 0x0000000F;

    // The keyword every edible item carries (VendorItemFood).
    public const uint VendorItemFood = 0x000A0E56;

    // The keyword on potions (VendorItemPotion).
    public const uint VendorItemPotion = 0x000A0E59;
}

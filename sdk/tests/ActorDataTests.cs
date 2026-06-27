using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the record-backed query data exposed to mods: an actor's sex and race
// (read through its base) and a weapon's base damage.
public static class ActorDataTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong actor = 0x14;
        const ulong actorBase = 0x7;
        const ulong race = 0x13745;  // NordRace
        fake.SetBase(actor, actorBase, essential: false);
        fake.SetActorBaseData(actorBase, sex: 1, race: race);

        Actor character = Actor.From(actor);
        check.Equal("sex read through the base", Sex.Female, character.Sex);
        check.Equal("race read through the base", race, character.Race.Handle);

        const ulong sword = 0x12EB7;
        fake.SetWeaponDamage(sword, 7);
        check.Equal("weapon damage from the record", 7, Form.From(sword).WeaponDamage);

        const ulong armor = 0x12E4D;
        fake.SetArmorRating(armor, 15f);
        check.Equal("armor rating from the record", 15f, Form.From(armor).ArmorRating);

        // Enchantment: an enchanted item resolves to its enchantment, a plain one
        // to None.
        const ulong enchantedSword = 0x17288;
        const ulong enchantment = 0x45C2C;
        fake.SetEnchantment(enchantedSword, enchantment);
        check.Equal("enchantment from the record", enchantment, Form.From(enchantedSword).Enchantment.Handle);
        check.That("enchanted item reports enchanted", Form.From(enchantedSword).IsEnchanted);
        check.That("plain item is not enchanted", !Form.From(sword).IsEnchanted);

        Native.Backend = null;
    }
}

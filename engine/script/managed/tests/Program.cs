using System;

namespace Recreation.Tests;

// The managed test runner: executes each suite against a shared Check, prints a
// summary and exits with the failure count so CI (and `dotnet run`) gate on it.
internal static class Program
{
    private static int Main()
    {
        var check = new Check();

        ValueTests.Run(check);
        SdkApiTests.Run(check);
        InventoryTests.Run(check);
        ActorDataTests.Run(check);
        WrapperDispatchTests.Run(check);
        ProximityTests.Run(check);
        EventBusTests.Run(check);
        EngineEventsTests.Run(check);
        ModHostTests.Run(check);
        ModLoaderTests.Run(check);
        ModConfigTests.Run(check);
        SchedulerTests.Run(check);
        CoroutineTests.Run(check);
        GateTests.Run(check);
        EffectsTests.Run(check);
        CooldownsTests.Run(check);
        FormScriptsTests.Run(check);
        SkyrimRegenTests.Run(check);
        SkyrimEventTests.Run(check);
        TimeOfDayTests.Run(check);
        SurvivalNeedsTests.Run(check);
        EncumbranceTests.Run(check);
        FastTravelTests.Run(check);
        LocationDiscoveryTests.Run(check);
        HarvestingTests.Run(check);
        MeditationPowerTests.Run(check);
        BarteringTests.Run(check);
        AlchemyTests.Run(check);
        IngredientDiscoveryTests.Run(check);
        CraftingTests.Run(check);
        LeveledListTests.Run(check);
        LeveledSpawnTests.Run(check);
        BookTests.Run(check);
        PotionTests.Run(check);
        EnchantingTests.Run(check);
        ZoneTests.Run(check);
        SpellTests.Run(check);
        PowersTests.Run(check);
        AbilitiesTests.Run(check);
        StandingStonesTests.Run(check);
        DiseasesTests.Run(check);
        ShrinesTests.Run(check);
        VampirismTests.Run(check);
        PickpocketingTests.Run(check);
        KeywordsTests.Run(check);
        FormDataTests.Run(check);
        RelationshipsTests.Run(check);
        FactionsTests.Run(check);
        FactionRelationsTests.Run(check);
        CrimeTests.Run(check);
        LootTests.Run(check);
        CarryWeightTests.Run(check);
        RaceTests.Run(check);
        RaceTraitsTests.Run(check);
        RacialAbilitiesTests.Run(check);
        EquipmentTests.Run(check);
        SmithingTests.Run(check);
        EnchantingPowerTests.Run(check);
        SoulGemTests.Run(check);

        Console.WriteLine($"[tests] {check.Passed} passed, {check.Failed} failed");
        return check.Failed;
    }
}

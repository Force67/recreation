using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Verifies the thin SDK wrappers (Quest, Faction, Cell, GlobalVariable, Scene)
// dispatch to the right native with the right arguments. These forward directly
// to engine natives, so a mistyped native name would silently fail; this catches
// that and keeps the wrappers honest against the registered native surface.
public static class WrapperDispatchTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        // Quest.
        var quest = Quest.From(0x100);
        _ = quest.Stage;
        check.Equal("Quest.Stage reads GetStage", "GetStage", fake.LastMethod);
        quest.Stage = 50;
        check.Equal("Quest.Stage writes SetStage", "SetStage", fake.LastMethod);
        check.Equal("SetStage passes the stage", 50, fake.LastMethodArg0.AsInt());
        _ = quest.IsRunning;
        check.Equal("Quest.IsRunning", "IsRunning", fake.LastMethod);
        quest.SetObjectiveCompleted(3);
        check.Equal("Quest objective completed", "SetObjectiveCompleted", fake.LastMethod);
        check.Equal("objective index passed", 3, fake.LastMethodArg0.AsInt());

        // Faction.
        var faction = Faction.From(0x200);
        _ = faction.GetReaction(Faction.From(0x201));
        check.Equal("Faction.GetReaction", "GetReaction", fake.LastMethod);
        faction.CrimeGold = 40;
        check.Equal("Faction.CrimeGold set", "SetCrimeGold", fake.LastMethod);
        faction.ModCrimeGold(5);
        check.Equal("Faction.ModCrimeGold", "ModCrimeGold", fake.LastMethod);

        // Cell.
        var cell = Cell.From(0x300);
        _ = cell.IsInterior;
        check.Equal("Cell.IsInterior", "IsInterior", fake.LastMethod);
        _ = cell.WaterLevel;
        check.Equal("Cell.WaterLevel reads GetWaterLevel", "GetWaterLevel", fake.LastMethod);

        // GlobalVariable.
        var global = GlobalVariable.From(0x400);
        global.Value = 1.5f;
        check.Equal("GlobalVariable.Value set", "SetValue", fake.LastMethod);
        _ = global.IntValue;
        check.Equal("GlobalVariable.IntValue reads GetValueInt", "GetValueInt", fake.LastMethod);
        global.Mod(2f);
        check.Equal("GlobalVariable.Mod", "Mod", fake.LastMethod);

        // Scene.
        var scene = Scene.From(0x500);
        _ = scene.OwningQuest;
        check.Equal("Scene.OwningQuest reads GetOwningQuest", "GetOwningQuest", fake.LastMethod);
        scene.ForceStart();
        check.Equal("Scene.ForceStart", "ForceStart", fake.LastMethod);
        _ = scene.IsPlaying;
        check.Equal("Scene.IsPlaying", "IsPlaying", fake.LastMethod);

        // ObjectReference world and lock methods.
        var reference = ObjectReference.From(0x600);
        var other = ObjectReference.From(0x601);
        reference.MoveTo(other);
        check.Equal("ObjectReference.MoveTo", "MoveTo", fake.LastMethod);
        _ = reference.Is3DLoaded;
        check.Equal("ObjectReference.Is3DLoaded", "Is3DLoaded", fake.LastMethod);
        reference.Scale = 2f;
        check.Equal("ObjectReference.Scale set", "SetScale", fake.LastMethod);
        reference.Activate(other);
        check.Equal("ObjectReference.Activate", "Activate", fake.LastMethod);
        reference.Lock();
        check.Equal("ObjectReference.Lock", "Lock", fake.LastMethod);
        _ = reference.LockLevel;
        check.Equal("ObjectReference.LockLevel reads GetLockLevel", "GetLockLevel", fake.LastMethod);
        _ = reference.OpenState;
        check.Equal("ObjectReference.OpenState reads GetOpenState", "GetOpenState", fake.LastMethod);
        _ = reference.DistanceTo(other);
        check.Equal("ObjectReference.DistanceTo reads GetDistance", "GetDistance", fake.LastMethod);

        // Actor state, combat and faction methods.
        var actor = Actor.From(0x700);
        _ = actor.Level;
        check.Equal("Actor.Level reads GetLevel", "GetLevel", fake.LastMethod);
        _ = actor.IsSneaking;
        check.Equal("Actor.IsSneaking", "IsSneaking", fake.LastMethod);
        _ = actor.CombatTarget;
        check.Equal("Actor.CombatTarget reads GetCombatTarget", "GetCombatTarget", fake.LastMethod);
        actor.EquipItem(Form.From(0x710));
        check.Equal("Actor.EquipItem", "EquipItem", fake.LastMethod);
        actor.AddSpell(Form.From(0x711));
        check.Equal("Actor.AddSpell", "AddSpell", fake.LastMethod);
        _ = actor.GetFactionRank(faction);
        check.Equal("Actor.GetFactionRank", "GetFactionRank", fake.LastMethod);
        actor.AddToFaction(faction);
        check.Equal("Actor.AddToFaction", "AddToFaction", fake.LastMethod);
        _ = actor.IsInFaction(faction);
        check.Equal("Actor.IsInFaction", "IsInFaction", fake.LastMethod);

        Native.Backend = null;
    }
}

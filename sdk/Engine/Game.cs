using Recreation.Interop;

namespace Recreation;

// The global entry point into the engine, mirroring the Papyrus Game script.
// Static because there is one game world: mod code starts here to reach the
// player, look up forms by id, and read or gate player input.
public static class Game
{
    // The player character.
    public static Actor Player => Actor.From(Global("GetPlayer").AsHandle());

    // Resolves a runtime (load-order) form id to a form, or Form.None if unknown.
    public static Form GetForm(uint formId) =>
        Form.From(Global("GetForm", (int)formId).AsHandle());

    // Typed lookups for when the caller knows the kind of the form at formId.
    public static Actor GetActor(uint formId) => Actor.From(GetForm(formId).Handle);
    public static ObjectReference GetReference(uint formId) =>
        ObjectReference.From(GetForm(formId).Handle);
    public static Quest GetQuest(uint formId) => Quest.From(GetForm(formId).Handle);
    public static Faction GetFaction(uint formId) => Faction.From(GetForm(formId).Handle);
    public static GlobalVariable GetGlobal(uint formId) =>
        GlobalVariable.From(GetForm(formId).Handle);

    // Real seconds elapsed, scaled to game hours, since the session began.
    public static float RealHoursPassed => Global("GetRealHoursPassed").AsFloat();

    public static float GetGameSettingFloat(string name) =>
        Global("GetGameSettingFloat", name).AsFloat();

    // --- player input gates ---------------------------------------------------
    // Cutscene-style control: which input categories the player currently has.
    public static bool MovementEnabled => Global("IsMovementControlsEnabled").AsBool();
    public static bool FightingEnabled => Global("IsFightingControlsEnabled").AsBool();
    public static bool FastTravelEnabled => Global("IsFastTravelEnabled").AsBool();

    public static void DisablePlayerControls() => Global("DisablePlayerControls");
    public static void EnablePlayerControls() => Global("EnablePlayerControls");
    public static void EnableFastTravel(bool enable = true) => Global("EnableFastTravel", enable);

    private static Value Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Game", function, args);
}

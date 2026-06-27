using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers per-form data: typed set/get, fallbacks, removal, per-form isolation, and
// the auto-clear when a form unloads.
public static class FormDataTests
{
    public static void Run(Check check)
    {
        FormData.Clear();
        EventBus.Clear();

        var chest = Form.From(0x100);
        var npc = Form.From(0x200);

        // Values round-trip typed.
        FormData.Set(chest, "looted", true);
        FormData.Set(chest, "visits", 3);
        FormData.Set(npc, "name", "Lydia");
        check.That("bool round-trips", FormData.Get<bool>(chest, "looted"));
        check.Equal("int round-trips", 3, FormData.Get<int>(chest, "visits"));
        check.Equal("string round-trips", "Lydia", FormData.Get<string>(npc, "name"));

        // A missing key, or a wrong-typed one, returns the fallback.
        check.Equal("missing key returns fallback", -1, FormData.Get(chest, "absent", -1));
        check.Equal("wrong type returns fallback", 0, FormData.Get<int>(chest, "looted"));

        // Has and Remove operate per key.
        check.That("Has finds a set key", FormData.Has(chest, "visits"));
        FormData.Remove(chest, "visits");
        check.That("Remove drops the key", !FormData.Has(chest, "visits"));
        check.That("other keys on the form survive", FormData.Has(chest, "looted"));

        // Forms are independent.
        check.Equal("two forms tracked", 2, FormData.TrackedForms);
        FormData.ClearForm(chest);
        check.That("ClearForm drops the form's data", !FormData.Has(chest, "looted"));
        check.That("the other form is untouched", FormData.Has(npc, "name"));

        // Data is dropped when the form unloads (the wiring ModHost installs).
        using var sub = EventBus.Subscribe<FormUnloaded>(e => FormData.ClearForm(e.Form));
        FormData.Set(npc, "asleep", true);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.FormUnloaded, A = npc.Handle });
        check.That("unloading a form drops its data", !FormData.Has(npc, "asleep"));

        FormData.Clear();
        check.Equal("Clear drops everything", 0, FormData.TrackedForms);

        EventBus.Clear();
    }
}

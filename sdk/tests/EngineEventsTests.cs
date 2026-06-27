using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Verifies the wire-format engine events translate to the right typed events on
// the bus, with their payloads intact. This is the contract the engine relies on
// when it raises events into managed code.
public static class EngineEventsTests
{
    public static void Run(Check check)
    {
        EventBus.Clear();

        ulong diedHandle = 0;
        using var s1 = EventBus.Subscribe<ActorDied>(e => diedHandle = e.ActorHandle);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.ActorDied, A = 0x11 });
        check.Equal("ActorDied maps with handle", 0x11UL, diedHandle);

        (ulong container, ulong item, int count) added = default;
        using var s2 = EventBus.Subscribe<ItemAdded>(e => added = (e.ContainerHandle, e.ItemHandle, e.Count));
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.ItemAdded, A = 0x22, B = 0x33, I = 5 });
        check.That("ItemAdded maps all payloads", added == (0x22UL, 0x33UL, 5));

        (ulong container, ulong item, int count) removed = default;
        using var s2b = EventBus.Subscribe<ItemRemoved>(e => removed = (e.ContainerHandle, e.ItemHandle, e.Count));
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.ItemRemoved, A = 0x22, B = 0x33, I = 2 });
        check.That("ItemRemoved maps all payloads", removed == (0x22UL, 0x33UL, 2));

        (ulong quest, int stage) staged = default;
        using var s3 = EventBus.Subscribe<QuestStageChanged>(e => staged = (e.QuestHandle, e.Stage));
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.QuestStageChanged, A = 0x44, I = 30 });
        check.That("QuestStageChanged maps payloads", staged == (0x44UL, 30));

        ulong loaded = 0;
        using var s4 = EventBus.Subscribe<FormLoaded>(e => loaded = e.FormHandle);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.FormLoaded, A = 0x55 });
        check.Equal("FormLoaded maps with handle", 0x55UL, loaded);

        ulong unloaded = 0;
        using var s4b = EventBus.Subscribe<FormUnloaded>(e => unloaded = e.FormHandle);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.FormUnloaded, A = 0x5A });
        check.Equal("FormUnloaded maps with handle", 0x5AUL, unloaded);

        ulong used = 0;
        using var s5 = EventBus.Subscribe<PlayerActivated>(e => used = e.TargetHandle);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.PlayerActivated, A = 0x66 });
        check.Equal("PlayerActivated maps with handle", 0x66UL, used);

        (ulong cell, bool interior) loc = default;
        using var s6 = EventBus.Subscribe<LocationChanged>(e => loc = (e.CellHandle, e.IsInterior));
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.LocationChanged, A = 0x77, I = 1 });
        check.That("LocationChanged maps cell and interior flag", loc == (0x77UL, true));

        Key key = Key.W;
        using var s7 = EventBus.Subscribe<KeyPressed>(e => key = e.Key);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.KeyPressed, A = (ulong)Key.F });
        check.Equal("KeyPressed maps the key code", Key.F, key);

        // Hotkeys.Bind fires only for its key.
        int fHits = 0, tHits = 0;
        using var bf = Hotkeys.Bind(Key.F, () => fHits++);
        using var bt = Hotkeys.Bind(Key.T, () => tHits++);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.KeyPressed, A = (ulong)Key.F });
        check.Equal("bound key fires", 1, fHits);
        check.Equal("other binding does not fire", 0, tHits);

        EventBus.Clear();
    }
}

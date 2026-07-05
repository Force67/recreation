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

        ulong used = 0;
        using var s5 = EventBus.Subscribe<PlayerActivated>(e => used = e.TargetHandle);
        EngineEvents.Dispatch(new ManagedEvent { Id = ManagedEventId.PlayerActivated, A = 0x66 });
        check.Equal("PlayerActivated maps with handle", 0x66UL, used);

        EventBus.Clear();
    }
}

using Recreation.Interop;

namespace Recreation.Modding;

// Translates raw engine events (the ManagedEvent wire payload the host delivers)
// into the typed events mods subscribe to, then publishes them on the bus. This
// is the one place the wire ids map to event types, so adding an event is a
// case here plus a type in GameEvent.cs.
public static class EngineEvents
{
    public static void Dispatch(in ManagedEvent e)
    {
        switch (e.Id)
        {
            case ManagedEventId.ActorDied:
                EventBus.Publish(new ActorDied(e.A));
                break;
            case ManagedEventId.ItemAdded:
                EventBus.Publish(new ItemAdded(e.A, e.B, e.I));
                break;
            case ManagedEventId.QuestStageChanged:
                EventBus.Publish(new QuestStageChanged((uint)e.A, e.I));
                break;
        }
    }
}

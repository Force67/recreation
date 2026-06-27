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
            case ManagedEventId.ItemRemoved:
                EventBus.Publish(new ItemRemoved(e.A, e.B, e.I));
                break;
            case ManagedEventId.QuestStageChanged:
                EventBus.Publish(new QuestStageChanged(e.A, e.I));
                break;
            case ManagedEventId.FormLoaded:
                EventBus.Publish(new FormLoaded(e.A));
                break;
            case ManagedEventId.FormUnloaded:
                EventBus.Publish(new FormUnloaded(e.A));
                break;
            case ManagedEventId.PlayerActivated:
                EventBus.Publish(new PlayerActivated(e.A));
                break;
            case ManagedEventId.LocationChanged:
                EventBus.Publish(new LocationChanged(e.A, e.I != 0));
                break;
            case ManagedEventId.KeyPressed:
                EventBus.Publish(new KeyPressed((Key)e.A));
                break;
            case ManagedEventId.ClientAssetsReady:
                EventBus.Publish(new ClientAssetsReady((uint)e.A));
                break;
            case ManagedEventId.ClientJoined:
                EventBus.Publish(new ClientJoined((uint)e.A));
                break;
            case ManagedEventId.ClientLeft:
                EventBus.Publish(new ClientLeft((uint)e.A));
                Rpc.DropPeerRequests((uint)e.A);  // a departed peer never replies
                break;
        }
    }
}

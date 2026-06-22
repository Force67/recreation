using System;
using System.Collections.Generic;

namespace Recreation.Modding;

// The publish/subscribe hub that makes the engine moddable: mods subscribe to
// game events by type and the engine (or other mods) publish them. Typed, so a
// handler for ActorDied never sees an ItemAdded, and extensible, since a mod can
// define and publish its own IGameEvent.
//
// Single-threaded by contract: the managed host runs on one thread, matching the
// guest, so subscription and publication need no locking. Handlers added or
// removed during a publish take effect on the next publish, not the current one
// (the listener list is snapshotted per publish).
public static class EventBus
{
    private static readonly Dictionary<Type, List<object>> Listeners = new();

    // Subscribes handler to events of type T. Dispose the returned token to stop
    // receiving them; keep it for the lifetime of the subscription.
    public static Subscription Subscribe<T>(Action<T> handler) where T : IGameEvent
    {
        ArgumentNullException.ThrowIfNull(handler);
        if (!Listeners.TryGetValue(typeof(T), out var list))
        {
            list = new List<object>();
            Listeners[typeof(T)] = list;
        }
        list.Add(handler);
        return new Subscription(typeof(T), handler);
    }

    // Publishes evt to every current subscriber of its type. A handler that
    // throws is isolated: it is reported and the rest still run.
    public static void Publish<T>(T evt) where T : IGameEvent
    {
        if (!Listeners.TryGetValue(typeof(T), out var list) || list.Count == 0)
            return;
        // Snapshot so a handler may (un)subscribe without disturbing this pass.
        var snapshot = list.ToArray();
        foreach (var entry in snapshot)
        {
            try
            {
                ((Action<T>)entry)(evt);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[events] handler for {typeof(T).Name} threw: {ex.Message}");
            }
        }
    }

    public static int ListenerCount<T>() where T : IGameEvent =>
        Listeners.TryGetValue(typeof(T), out var list) ? list.Count : 0;

    // Removes all subscriptions. Used when the mod host tears the managed world
    // down so a reload starts clean.
    public static void Clear() => Listeners.Clear();

    internal static void Remove(Type eventType, object handler)
    {
        if (Listeners.TryGetValue(eventType, out var list))
            list.Remove(handler);
    }

    // A handle to one subscription. Disposing it unsubscribes; it is safe to
    // dispose more than once.
    public sealed class Subscription : IDisposable
    {
        private readonly Type _eventType;
        private object? _handler;

        internal Subscription(Type eventType, object handler)
        {
            _eventType = eventType;
            _handler = handler;
        }

        public void Dispose()
        {
            if (_handler == null) return;
            Remove(_eventType, _handler);
            _handler = null;
        }
    }
}

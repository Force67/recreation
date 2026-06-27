using System;
using System.Collections.Generic;

namespace Recreation.Modding;

// The publish/subscribe hub that makes the engine moddable: mods subscribe to
// game events by type and the engine (or other mods) publish them. Typed, so a
// handler for ActorDied never sees an ItemAdded, and extensible, since a mod can
// define and publish its own IGameEvent.
//
// Handlers run in priority order (higher first), so cooperating mods can choose
// to react before or after others, the way gmod hook priorities let mods compose.
// Equal priorities keep subscription (FIFO) order.
//
// Single-threaded by contract: the managed host runs on one thread, matching the
// guest, so subscription and publication need no locking. Handlers added or
// removed during a publish take effect on the next publish, not the current one
// (the listener list is snapshotted per publish).
public static class EventBus
{
    private static readonly Dictionary<Type, List<Listener>> Listeners = new();

    // Subscribes handler at the default priority. Dispose the returned token to
    // stop receiving events; keep it for the lifetime of the subscription.
    public static Subscription Subscribe<T>(Action<T> handler) where T : IGameEvent =>
        Subscribe(handler, 0);

    // Subscribes handler at the given priority; higher priorities run earlier.
    public static Subscription Subscribe<T>(Action<T> handler, int priority) where T : IGameEvent
    {
        ArgumentNullException.ThrowIfNull(handler);
        if (!Listeners.TryGetValue(typeof(T), out var list))
        {
            list = new List<Listener>();
            Listeners[typeof(T)] = list;
        }
        // Insert after every handler of equal-or-higher priority, so the list
        // stays sorted by descending priority with FIFO order within a tier.
        int i = 0;
        while (i < list.Count && list[i].Priority >= priority) i++;
        list.Insert(i, new Listener(priority, handler));
        return new Subscription(typeof(T), handler);
    }

    // Publishes evt to every current subscriber of its type, in priority order. A
    // handler that throws is isolated: it is reported and the rest still run.
    public static void Publish<T>(T evt) where T : IGameEvent
    {
        if (!Listeners.TryGetValue(typeof(T), out var list) || list.Count == 0)
            return;
        // Snapshot so a handler may (un)subscribe without disturbing this pass.
        var snapshot = list.ToArray();
        foreach (Listener entry in snapshot)
        {
            try
            {
                ((Action<T>)entry.Handler)(evt);
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
            list.RemoveAll(l => ReferenceEquals(l.Handler, handler));
    }

    private readonly struct Listener
    {
        public readonly int Priority;
        public readonly object Handler;

        public Listener(int priority, object handler)
        {
            Priority = priority;
            Handler = handler;
        }
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

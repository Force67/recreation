using Recreation.Modding;

namespace Recreation.Tests;

// Verifies the publish/subscribe semantics mods rely on.
public readonly struct Ping(int n) : IGameEvent
{
    public int N { get; } = n;
}

public static class EventBusTests
{
    public static void Run(Check check)
    {
        EventBus.Clear();

        int sum = 0;
        var sub = EventBus.Subscribe<Ping>(p => sum += p.N);
        EventBus.Publish(new Ping(3));
        EventBus.Publish(new Ping(4));
        check.Equal("subscriber receives each publish", 7, sum);
        check.Equal("listener counted", 1, EventBus.ListenerCount<Ping>());

        sub.Dispose();
        EventBus.Publish(new Ping(100));
        check.Equal("disposed subscriber stops receiving", 7, sum);
        check.Equal("listener removed on dispose", 0, EventBus.ListenerCount<Ping>());

        // A throwing handler must not stop the others.
        int reached = 0;
        EventBus.Subscribe<Ping>(_ => throw new System.InvalidOperationException("boom"));
        EventBus.Subscribe<Ping>(_ => reached++);
        EventBus.Publish(new Ping(1));
        check.Equal("a throwing handler is isolated", 1, reached);

        // Typed isolation: a Pong handler never sees a Ping.
        int pongs = 0;
        EventBus.Subscribe<Pong>(_ => pongs++);
        EventBus.Publish(new Ping(1));
        check.Equal("handlers are typed", 0, pongs);

        EventBus.Clear();
        check.Equal("clear removes all", 0, EventBus.ListenerCount<Ping>());
    }

    public readonly struct Pong : IGameEvent;
}

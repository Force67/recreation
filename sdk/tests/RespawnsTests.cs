using Recreation;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Covers what a session does with a dead player when no ruleset has said. The
// engine only marks the death; this waits out the delay and asks the engine to
// put them back on their feet, and it gets out of the way when something else
// revives them first or when a ruleset turns it off.
public static class RespawnsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        Platform.Boot(NetRole.Server);
        Respawns.Delay = 2f;

        // A death starts the count, and nothing happens until it runs out.
        EventBus.Publish(new PlayerVitalsChanged(netId: 7, health: 0, maxHealth: 100, dead: true,
                                                 peer: 3));
        EventBus.Publish(new FrameUpdate(1f));
        check.Equal("nothing respawns before the delay", 0u, fake.LastRespawnedPeer ?? 0u);
        EventBus.Publish(new FrameUpdate(1.5f));
        check.Equal("the player is put back on their feet after it", 3u, fake.LastRespawnedPeer ?? 0u);

        // ...once, not every frame after.
        fake.ClearRespawn();
        EventBus.Publish(new FrameUpdate(5f));
        check.Equal("and only once", 0u, fake.LastRespawnedPeer ?? 0u);

        // Revived by other means (a mod's SetHealth): the pending respawn drops.
        EventBus.Publish(new PlayerVitalsChanged(netId: 7, health: 0, maxHealth: 100, dead: true,
                                                 peer: 4));
        EventBus.Publish(new PlayerVitalsChanged(netId: 7, health: 100, maxHealth: 100, dead: false,
                                                 peer: 4));
        EventBus.Publish(new FrameUpdate(10f));
        check.Equal("a player revived another way is not respawned again", 0u,
                    fake.LastRespawnedPeer ?? 0u);

        // A ruleset can take it over entirely.
        Respawns.Enabled = false;
        EventBus.Publish(new PlayerVitalsChanged(netId: 7, health: 0, maxHealth: 100, dead: true,
                                                 peer: 5));
        EventBus.Publish(new FrameUpdate(10f));
        check.Equal("turning it off leaves the dead alone", 0u, fake.LastRespawnedPeer ?? 0u);
        Respawns.Enabled = true;

        // A client owns nobody's body, so it never respawns anyone.
        Platform.Boot(NetRole.Client);
        fake.ClearRespawn();
        EventBus.Publish(new PlayerVitalsChanged(netId: 7, health: 0, maxHealth: 100, dead: true,
                                                 peer: 6));
        EventBus.Publish(new FrameUpdate(10f));
        check.Equal("a client respawns nobody", 0u, fake.LastRespawnedPeer ?? 0u);

        ModHost.Shutdown();
    }
}

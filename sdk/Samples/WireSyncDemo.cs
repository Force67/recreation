using System;
using Recreation.Modding;

namespace Recreation.Net.Samples;

// Two-process demo of native multiplayer sync: the server publishes shared state
// and chat, the client logs what it receives over the wire. Gated on REC_WIRE_DEMO.
[Mod("WireSyncServer"), Realm(ModRealm.Server)]
public sealed class WireSyncServer : IMod
{
    public void OnLoad()
    {
        if (Environment.GetEnvironmentVariable("REC_WIRE_DEMO") == null) return;
        Console.WriteLine("[wiresync/server] online; publishing shared state");
        StateBags.Global.Set("motd", "Synced over the wire by recreation");

        EventBus.Subscribe<ClientJoined>(e =>
        {
            Console.WriteLine($"[wiresync/server] client {e.Peer} joined; updating state");
            StateBags.Global.Set("last_join", (int)e.Peer);
            Chat.System($"Player {e.Peer} connected.");
        });
        EventBus.Subscribe<ClientAssetsReady>(e =>
            Chat.System("Welcome. The world state is synced."));
    }
}

[Mod("WireSyncClient"), Realm(ModRealm.Client)]
public sealed class WireSyncClient : IMod
{
    public void OnLoad()
    {
        if (Environment.GetEnvironmentVariable("REC_WIRE_DEMO") == null) return;
        Console.WriteLine("[wiresync/client] online; watching for replicated state");

        StateBags.Global.OnChange("motd", c =>
            Console.WriteLine($"[wiresync/client] received motd over the wire: '{c.Value.AsString()}' " +
                              $"(fromServer={c.FromServer})"));
        StateBags.Global.OnChange("last_join", c =>
            Console.WriteLine($"[wiresync/client] server reports last_join={c.Value.AsInt()}"));
        Chat.OnMessage(m =>
            Console.WriteLine($"[wiresync/client] chat received over the wire: {m.Text}"));
        EventBus.Subscribe<PlayerJoined>(e =>
            Console.WriteLine($"[wiresync/client] roster synced: player {e.Player.Id} present"));
    }
}

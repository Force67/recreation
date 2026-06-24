# Modding guide: streamed resources and multiplayer scripting

A server ships content and behaviour to its players the way FiveM does: drop a
resource in the server mods directory and it is distributed automatically, and a
C# mod drives the session over RPC. This is the end-to-end workflow.

## 1. A streamable resource

The server points at a mods directory (`--mods-dir ./server_mods`). Each immediate
subdirectory is a resource; the files inside are what clients receive, laid out
the way the game resolves them:

```
server_mods/
  bigswords/
    meshes/weapons/greatsword.nif
    textures/weapons/greatsword.dds
    .streamignore          # optional: files to keep on the server
    server/balance.cfg      # kept server-side by the .streamignore below
```

Every file is content-hashed into a manifest. On join a client diffs the manifest
against its cache and pulls only what it is missing, deduped by content, resumable
across reconnects. The host mounts the same resource itself, so a listen server
sees exactly what it streams.

### Keeping files server-side

List files a resource must not ship to clients in a `.streamignore` at the resource
root (gitignore flavored). They never enter the manifest, so the server cannot even
be asked for them:

```
server/          # a whole directory
balance.cfg      # an exact file
*.bak            # an extension
```

### Checking what ships

Before deploying, run `modtool inspect` over the mods directory to see exactly what
each client will receive and confirm the `.streamignore` kept the right files back:

```sh
modtool inspect ./server_mods
```

## 2. A server mod

A C# mod (built into `Recreation.Scripting`, or dropped in via
`RECREATION_MODS_DIR`) reacts to the session and answers clients authoritatively.
Mods default to the `Server` realm, so they run on the host only.

```csharp
using Recreation.Modding;
using Recreation.Interop;

[Mod("BigSwords"), Realm(ModRealm.Server)]
public sealed class BigSwordsServer : IMod
{
    public void OnLoad()
    {
        // Greet a player once their resources have finished streaming.
        EventBus.Subscribe<ClientAssetsReady>(e =>
            Rpc.ToClient(e.Peer, "welcome", Value.String("Big swords loaded!")));

        EventBus.Subscribe<ClientLeft>(e => /* clean up per-player state */ ());

        // Answer an authoritative ask-and-answer request from a client.
        Rpc.OnRequest("may_i_spawn", req =>
            req.Reply(Value.Bool(/* server-side check */ true)));
    }
}
```

## 3. A client mod

A `Client` (or `Shared`) realm mod runs on connecting players. It cannot mutate
authoritative state (those calls are gated), so it handles UI, local effects and
requests to the server.

```csharp
[Mod("BigSwordsUi"), Realm(ModRealm.Client)]
public sealed class BigSwordsClient : IMod
{
    public void OnLoad()
    {
        Rpc.On("welcome", e => Debug.Log(e.Args[0].AsString()));
        // Ask the server, then act on its answer.
        Rpc.Request("may_i_spawn", new[] { Value.Object(playerHandle) }, reply =>
        {
            if (reply[0].AsBool()) /* play a local effect */;
        });
    }
}
```

## 4. Running it

```sh
# Host
recreation-server --data-dir <game Data> --mods-dir ./server_mods --port 29700

# Player
recreation --connect <host> --asset-cache ./cache
```

The player downloads `bigswords`, mounts it, runs the `Client`/`Shared` mods, and
talks to the host's `Server` mods over RPC. The host runs its `Server` mods and
the authoritative simulation.

## 5. Iterating live

After editing files in the mods directory, reload them without restarting the
server. Already-connected players receive the change live: each re-diffs against
its cache, streams only the files that changed, and re-mounts, no rejoin needed.

```sh
kill -HUP <server-pid>
```

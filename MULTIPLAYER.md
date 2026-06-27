# The Recreation multiplayer platform

recreation is a FiveM-class multiplayer platform on a modern raytraced engine: a
server ships content and behaviour to players, the world is server-authoritative,
and mods written in C# drive everything from gameplay rules to UI. This document
is the map of the platform layer that makes a server a *place* players return to,
and how its pieces fit so contributors extend it without stepping on each other.

## What already exists (the substrate)

- **Asset streaming** (`engine/modstream`, `engine/net/asset_stream`): a server
  distributes its UGC to joining players, content-hashed and resumable.
- **Scripting RPC** (`engine/rpc`, `Recreation.Modding.Rpc`): a typed, bidirectional
  channel. `Emit` (client→host), `ToClient`/`Broadcast` (host→clients), `On`,
  plus request/response. Realm-aware (`Server`/`Client`/`Shared`).
- **Server-authoritative replication** (`engine/net`): ECS snapshots, NPC actor
  sync, quests, world commands. The host simulates; clients render the truth.
- **Managed gameplay SDK** (`sdk/`): Game/Form/Actor/Quest and a
  deep per-game ruleset (Skyrim/Fallout/Starfield), EventBus, behaviours.

## The platform layer (`Recreation.Net`)

Built on the substrate, riding the existing RPC channel so most of it needs **no
new wire protocol**. One boot/reset seam (`Platform.Boot`/`Reset`, wired in
`ScriptHost` and `ModHost`) brings every subsystem up for the process role.

- **`Platform`** — role (`Server`/`Client`/`Standalone`) and the boot/reset hub.
- **State bags** (`StateBags`, `StateBag`) — server-authoritative replicated
  key/value state, scoped to the world (`global`), a player (`player:<id>`) or an
  entity (`entity:<netid>`). The foundational networked-state primitive: a server
  set broadcasts; a client set is a validated request; a joiner is full-synced.
  Change handlers (`OnChange`, `OnAnyChange`) let systems react locally.
- **Players** (`Players`, `Player`) — the roster, identity and per-player state.
  The roster is built by watching `player:*` presence, so it is identical on host
  and clients. `Players.Local`, `Players.All`, `Players.Get(id)`, and the
  `PlayerJoined`/`PlayerLeft` events that fire everywhere (unlike the server-only
  engine `ClientJoined`/`ClientLeft`).

### Conventions for new subsystems

- A subsystem is its own folder under `sdk/` and a test file
  under `tests/` (auto-discovered, no runner edits).
- Replicate over RPC with a **namespaced name** (`chat:*`, `social:*`, `admin:*`,
  `map:*`) or, better, hang state off a **state bag** so it replicates automatically.
- Be server-authoritative: a client *requests*, the server *decides* and the truth
  flows back. Use `Platform.IsServer` to gate authority.
- Render through the no-op-safe HUD natives (`Native.CallGlobal("Hud", ...)`); the
  logic stays headless-testable, the visuals light up when the engine HUD is wired.
- Wire boot/reset into `Platform` so a session reload starts clean.

## Roadmap

Shipped (C#, all tested in the managed runner):

- Test auto-discovery, **state bags**, **player registry**.
- **Chat** (`Chat/`) — channels, `/commands`, server-authoritative routing.
- **Social** (`Social/`) — parties, presence.
- **Admin** (`Admin/`) — ACE permissions, gated admin commands.
- **Persistence** (`Persistence/`) — KV stores + per-player save/load.
- **HUD framework** (`Hud/`) — prompts, notifications, data-driven menus.
- **Map** (`Map/`) — networked blips, waypoints.
- **Scoreboard** (`Scoreboard/`), **Server browser** (`Browser/`).

Also shipped (C#): **Voice** (positional, channels, proximity mix), **Economy**
(wallets/jobs/shops), **Teams** (factions/score/friendly fire), **NetEntities**
(networked spawnable objects + ownership), **Nametags**, and a **sample roleplay
gamemode** (`Samples/RoleplayServer`) composing teams + economy + chat commands.

Shipped (engine): the **game-agnostic platform HUD bridge** (`runtime/
platform_hud.*`, natives in `PapyrusGuest::BindEngineNatives`). Six on-screen
channels render from it: **chat box**, **scoreboard**, **interaction prompts**,
**compass blips**, **notifications**, and **floating player nametags** (world
position projected to screen). Plus `Net.LocalWorldPos` (the local player's world
position for mods) and `PlaceObject`-based net-entity placement. Verified live in a
streamed scene (`REC_MP_DEMO`).

Planned:

- A real two-process listen-server + client demo (the roster is faked in the
  single-player showcase today; the sync logic is loopback-tested).
- Per-model net-entity meshes (resolve `op.model` to a form/mesh), richer avatar
  sync, in-world health bars.

## Building and testing the managed layer

The managed tests are self-contained and gate on the failure count:

```sh
nix develop \
  --override-input zetanet-src path:/path/to/zetanet \
  --override-input nanobuf-src path:/path/to/nanobuf \
  --command bash -c 'cd sdk/tests && dotnet run -c Release'
```

(On this box, `./run-local.sh '...'` wraps the same overrides.)

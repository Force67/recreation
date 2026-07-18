# recreation

A modern, ECS driven game engine that loads Bethesda game content (Skyrim SE,
Fallout 4, Fallout 76) and plays it on a Vulkan rendering stack.

The engine never renders or simulates Bethesda data directly. ESM/ESL plugins,
BSA/BA2 archives, NIF meshes and legacy materials are converted into engine
native formats at load time. Everything downstream (renderer, world streaming,
networking) only knows engine formats.

## Layout

| Module | Purpose |
| --- | --- |
| `engine/core` | platform, logging, jobs, timing, window |
| `engine/ecs` | archetype based ECS, written from scratch |
| `engine/asset` | engine native asset formats, VFS, asset database |
| `engine/render` | Vulkan RHI, render graph, TAA, upscalers, raytracing |
| `engine/bethesda` | ESM/ESL/BSA/BA2/NIF readers and converters |
| `engine/world` | cell streaming and gameplay components |
| `engine/net` | server authoritative replication of ECS state |
| `engine/modstream` | content-addressed mod catalog, cache and Vfs mount |
| `engine/rpc` | typed scripting RPC value, wire codec and registry |
| `engine/script` | scripting host, per game Papyrus adapters |
| `runtime` | entry point and main loop |
| `tools` | offline tooling |

### Terrain editing

The in-game map editor's Terrain tool writes a compact non-destructive
`.recterrain` height diff; Bethesda plugins and archives are never rewritten.
Raise, lower, smooth, and flatten strokes update streamed LAND meshes, ground
queries, and colliders live, with each drag grouped into one undo operation.

![Recreation LAND height editing over streamed Skyrim content](docs/images/terrain-editor.png)

Terrain and object layout data load on first editor entry and save together with
F5 or the Save toolbar button. Set `REC_TERRAIN_EDITS` to choose the diff path;
otherwise it sits beside `editor_layout.reclayout` with a world-specific
`editor_layout.<world>_<hash>.recterrain` name. The binary format and
source-fingerprint rules are documented in
[`engine/world/README.md`](engine/world/README.md). For automated captures or a
terrain-first authoring session, `RX_EDITOR=1` opens the editor at startup and
`RX_EDITOR_TERRAIN=1` selects the Raise tool.

## Building

New box? The setup scripts take an unknown machine to a buildable state:
install the toolchain and shader compilers, fetch the third-party deps, clone
the sibling repos (rx, zetanet, libultragui) and report anything still missing.
Building recreation requires a sibling rx checkout (the engine); its SDK deps
(FidelityFX/DLSS/NRD/Jolt) are fetched into that checkout by
`../rx/tools/get_*.sh`. Point at an rx elsewhere with
`-DRECREATION_RX_DIR=/path/to/rx`.

```sh
scripts/setup.sh                 # Linux/macOS: do everything
scripts/setup.sh --check         # report only, change nothing
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup-windows.ps1
```

These are the same scripts CI uses, so the dependency set never drifts from
what the build actually needs. Then configure and build:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Vulkan headers and the volk loader are pinned and fetched at configure time,
no SDK install needed. At runtime a Vulkan 1.3 driver is required for
rendering, without one (or without a window) the renderer degrades to a stub.
SDL3 and zlib stay optional at build time: without SDL3 the runtime is
headless (pass `-DRECREATION_FETCH_SDL3=ON` to download it), without zlib
compressed plugin records are rejected at load time.

Targets: Windows, Linux, Android (via the NDK toolchain file).

With Nix, `nix develop` provides the toolchain, SDL3, the Vulkan loader,
validation layers and tools. Configure with the pinned dependency set via
`cmake -B build/nix -G Ninja $RECREATION_FETCHCONTENT_FLAGS`, and launch
Vulkan binaries through the `vkrun` wrapper so the loader and the host GPU
driver are found. `nix build` produces a hermetic build from the same pins.

## Mods

Mod compatibility follows the same rules the original games use. Plugins merge
records with last loaded winning, loose files override archives, and mount
order in the VFS decides priority. Papyrus is game specific and handled by per
game adapters in `engine/script`.

### Multiplayer asset streaming

A server distributes its own UGC to joining players, FiveM style. Point the host
at a mods directory whose immediate subdirectories are resources:

```sh
recreation-server --mods-dir ./server_mods --port 29700
```

The host catalogs every file (content hashed) and offers the manifest on join.
Files a resource should keep server-side (configs, source data, secrets) stay off
clients by listing them in a `.streamignore` at the resource root, gitignore
style (`server/`, an exact path, or `*.ext`); excluded files never enter the
catalog, so the server cannot even be asked for them.

The host mounts its own catalogued mods too, so a listen server sees exactly the
content it streams to clients. A connecting client diffs the manifest against its
local cache, pulls only the content it is missing over the reliable file
transporter, verifies it, and mounts the resources into its asset Vfs so the
host's custom meshes, textures and scripts resolve like loose files:

```sh
recreation --connect <host> --asset-cache ./cache
```

### Scripting RPC

Server-side mod scripts drive multiplayer through a typed RPC channel. A C# mod
calls `Rpc.Emit(name, args)` (client to host), `Rpc.ToClient(peer, name, args)`
or `Rpc.Broadcast(name, args)` (host to clients), and subscribes with
`Rpc.On(name, e => ...)`. For the ask-and-answer case a client calls
`Rpc.Request(name, args, reply => ...)` and the server answers authoritatively
with `Rpc.OnRequest(name, req => req.Reply(...))`. Calls ride the session's
reliable channel. The host also raises a `ClientAssetsReady` event once a player
has finished streaming the server's mods, so a mod can hold the player until
their UGC has arrived:

```csharp
EventBus.Subscribe<ClientAssetsReady>(e =>
    Rpc.ToClient(e.Peer, "welcome", Value.String("Mods loaded, have fun!")));
```

A mod declares which side it runs on with `[Realm(ModRealm.Server|Client|Shared)]`
(no tag means Server). A host starts its Server and Shared mods, a connecting
client starts Client and Shared mods, and single-player runs everything, so
authoritative gameplay stays on the server while client mods handle UI, local
effects and `Rpc.Emit` requests. Authoritative mutations a client mod attempts
are gated, so it cannot diverge from the server.

See [MODDING.md](MODDING.md) for a complete worked example: a streamed resource, a
server mod and a client mod, wired together.

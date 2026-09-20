# recreation

A modern, ECS driven game engine that loads Bethesda game content (Skyrim SE,
Fallout 4, Fallout 76) and plays it on a Vulkan rendering stack.

The engine never renders or simulates Bethesda data directly. ESM/ESL plugins,
BSA/BA2 archives, NIF meshes and legacy materials are converted into engine
native formats at load time. Everything downstream (renderer, world streaming,
networking) only knows engine formats.

## Layout

The engine itself (core, ECS, asset formats, Vulkan renderer, networking) lives
in the sibling `rx` repo. This repo is the Bethesda content layer and the game
runtime on top of it.

| Module | Purpose |
| --- | --- |
| `components/bethesda` | ESM/ESL/BSA/BA2/NIF readers and converters |
| `components/swf` | Flash/Scaleform readers, ActionScript decompiler, ugui translation |
| `components/world` | cell streaming and gameplay components |
| `components/quest` | quest definitions, stages, objectives and tracking |
| `components/dialogue` | dialogue topics, responses and scene playback |
| `components/audio` | ambient beds and sound catalog |
| `components/weather` | WTHR/CLMT climate selection driving the physical sky |
| `components/script` | scripting host, per game Papyrus and Obscript adapters |
| `components/modstream` | content-addressed mod catalog, cache and Vfs mount |
| `components/gamenet` | game side replication over rx's net layer |
| `runtime` | entry point, main loop, UI, editor, actors |
| `sdk` | C# modding and multiplayer platform |
| `tools` | offline tooling |

### Terrain editing

The in-game map editor's Terrain tool writes a compact non-destructive
`.recterrain` height diff; Bethesda plugins and archives are never rewritten.
Raise, lower, smooth, and flatten strokes update streamed LAND meshes, ground
queries, and colliders live, with each drag grouped into one undo operation.

Terrain and object layout data load on first editor entry and save together with
F5 or the Save toolbar button. Set `REC_TERRAIN_EDITS` to choose the diff path;
otherwise it sits beside `editor_layout.reclayout` with a world-specific
`editor_layout.<world>_<hash>.recterrain` name. The binary format and
source-fingerprint rules live in `components/world/terrain_edits.h`. For
automated captures or a terrain-first authoring session, `RX_EDITOR=1` opens the
editor at startup and `RX_EDITOR_TERRAIN=1` selects the Raise tool.

## Building

New box? The setup scripts take an unknown machine to a buildable state:
install the toolchain and shader compilers, fetch the third-party deps, clone
the sibling repos (rx, zetanet, libultragui) and report anything still missing.
Building recreation requires a sibling rx checkout (the engine); its SDK deps
(FidelityFX/DLSS/NRD/Jolt) are fetched into that checkout by
`../rx/tools/get_*.sh`. Point at an rx elsewhere with
`-DRECREATION_RX_DIR=/path/to/rx`. libultragui is required too, and not only
for the HUD: rx draws its engine splash with it (`rx::ui`), so every build
needs the checkout even where no HUD is built.

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
A windowed run opens on the rx engine splash for a couple of seconds; `RX_SPLASH=0`
suppresses it, and a headless or fixed-timestep run (every capture) never shows it.
SDL3 and zlib stay optional at build time: without SDL3 the runtime is
headless (pass `-DRECREATION_FETCH_SDL3=ON` to download it), without zlib
compressed plugin records are rejected at load time.

Targets: Windows, Linux, Android (via the NDK toolchain file). The Windows
binary can also be cross-built from Linux and run here, which is how the port
is tested: see [`WINDOWS.md`](WINDOWS.md).

With Nix, `nix develop` provides the toolchain, SDL3, the Vulkan loader,
validation layers and tools. Configure with the pinned dependency set via
`cmake -B build/nix -G Ninja $RECREATION_FETCHCONTENT_FLAGS`, and launch
Vulkan binaries through the `vkrun` wrapper so the loader and the host GPU
driver are found. `nix build` produces a hermetic build from the same pins.

## The games' own interface

The menus every Bethesda game ships are Scaleform movies. `tools/swfdump` reads
them: the tag stream, the vector art, the bitmaps, the text fields and the
ActionScript behind all of it. Skyrim's ActionScript 2 decompiles back to
source; Fallout 4 and Starfield use ActionScript 3, whose classes and method
bodies come back as a full symbol outline plus disassembly.

```sh
swfdump 'Interface/hudmenu.swf'                    # what the movie contains
swfdump 'Interface/hudmenu.swf' --script           # the ActionScript behind it
swfdump 'Interface/hudmenu.swf' --text             # fields and their bindings
swfdump 'Interface/hudmenu.swf' --fonts out/       # its embedded typeface, as TTF
swfdump --data "$SKYRIM/Data" --ugui-all out/ 1.5  # every menu at once
```

`--ugui-all` mounts the game's archives and translates the whole interface: a
`.ugui` screen per menu, the SVG and PNG art it references, a manifest binding
that art to widgets, the decompiled ActionScript beside it, and the game's own
typeface converted to TrueType. Text comes out in the player's language and in
Skyrim's own Futura Condensed, and a menu the game splices together from several
movies is spliced together here too. The scale argument fits Bethesda's 720p
stage to the engine's 1080p ui space.

The engine loads the result like any other screen:

```sh
RX_VANILLA_UI=hudmenu,quest_journal ./run-skyrim.sh
```

Set `RX_VANILLA_UI_DIR` to load them from somewhere other than
`runtime/ui/vanilla`. The translated screens are derived from an installed game,
so they are generated locally and never checked in.

## Mods

Mod compatibility follows the same rules the original games use. Plugins merge
records with last loaded winning, loose files override archives, and mount
order in the VFS decides priority. Papyrus is game specific and handled by per
game adapters in `components/script`.

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

#### Client scripts

A resource can also ship *code*: list managed assemblies in a `client_scripts.txt`
at the resource root (one resource-relative path per line, `#` comments) and the
server offers them to every joining client as client scripts. The assemblies
stream like any other file, hash-verified into the cache; whether they run is the
client's choice. On the first join of a server the loading screen asks:

> This server runs custom code (2 assembly(ies)) — [1] run once · [2] always for
> this server · [3] don't run

"Always" and "don't run" are remembered per server in `script_trust.ini` beside
`setup.ini`. `net.stream_scripts` overrides the whole gate: `0` never runs
streamed code, `1` (default) asks as above, `2` runs everything without asking.
Accepted assemblies load into the engine's own runtime after the streamed content
mounts, filtered by `[Realm]` like every other mod, so `[Realm(Client)]` and
`[Realm(Shared)]` code runs on the client exactly as a locally installed mod
would. Dependencies resolve from the same streamed batch regardless of load
order. Because the runtime never unloads assemblies, an assembly the server
re-streams under new bytes applies on the next join.

Streamed scripts run with the full trust of any other mod — the consent prompt
is permission, not a sandbox.

### One world, and who owns it

A co-op mod bolted onto one of these games has no choice but to hook a local
simulation and patch sync over it, which is why those projects spend their lives
chasing divergence. This is a reimplementation, so the rule is structural
instead: **the host is the world, and a client is a view of it.**

Every machine knows which it is (`EngineContext::Authority`, one of standalone,
host or replica) and asks one question, `simulates()`. A replica runs none of the
authoritative world simulation: not NPC AI, steering or ambient sandbox, not the
combat driver, not AI packages, not scripted trigger boxes, not quest-driven
world mutation. It receives the results instead (transforms and deaths through
actor sync, journal state through quest replication, the clock and sky through
world state) and anything its player does travels to the host as a request the
host answers. Two things stay local on purpose: the navmesh, because it derives
from static collision rather than world state and the player's auto-walk paths
over it, and the cutscene director, which plays the scenes the host started.

Forgetting is the failure mode, so it is guarded rather than trusted. Every
script-driven world mutation (spawn, move, enable, delete, combat enrollment,
teleporting the player) funnels through one sink, and on a replica that sink
drops what arrives and counts it, saying so in the log. A system that starts
simulating where it should not is a line in the log rather than a desync somebody
reports a week later.

One system is not authoritative yet, and the README says so where it lives:
dropped items are still local to whichever machine dropped them, so two players
do not see one world's loot. That is the next piece.

### Player sync

Players are real bodies, not placeholder cubes. Movement is server-simulated: a
client streams its character intent (world-space move request, look yaw, jump /
crouch / gait) to the host every tick, the host runs the same character pipeline
the local player uses (same MOVT-decoded speeds, gravity and capsule), and every
client receives the authoritative transforms. Remote bodies render with the
game's own assets — the player template, FaceGen head, armour and the full
idle/walk/run locomotion machine — and their gait animates from the replicated
velocity, so a remote player walks when they walk and stops when they stop. The
listen host has a body on the wire too, so clients can see the host.

A client simulates its own body locally as well, so input feels instant rather
than a round trip late, which leaves two copies of one body free to drift apart:
the host applies your intent half a round trip late, a lost packet skips an
intent, and a collision resolved a frame apart puts you on different sides of a
rock. The client reconciles them, reluctantly, because chasing every small
disagreement is what rubber-banding is: a gap under a quarter metre is left
alone, a walkable one is closed smoothly over a few frames, and only a gap too
large to walk off is snapped. Height is judged separately and never eased, so a
jump the host has not applied yet is not mistaken for an error, while falling
through the world still is. `net.reconcile 0` turns the correction off.

Vitals replicate too: every client receives them (a `PlayerVitals` component on
the player's replica and a `PlayerVitalsChanged` event for mods), and the dead
flag drives the client-side `Dead` tag. Host-side mod code sets them with
`Player.SetHealth(health, max, dead)`; so does combat.

### Fighting

A swing is a request, not a result. A client cannot be trusted to say what it
hit, and does not even know where anyone really is, since the host simulates
every body -- so it sends the aim it swung along and the host resolves that
against the transforms it owns, with the same reach, arc and damage the
single-player melee driver uses. The host accepts one swing per melee cadence per
player and drops the rest, which is the rate limit and the game rule at once:
nobody swings faster than the animation, so spamming the message buys nothing.

What connects takes damage where it lives. Another player's blow lands on the
health pool that already replicates, so every client sees it and the kill sets
the dead flag; an NPC takes it through the guest thread, so `OnHit`, `OnDeath`
and every quest watching them run exactly as in single player. A joining player
starts with a pool of 100 and a swing removes 42, both of which mod code can
replace through `Player.SetHealth`. `net.pvp 0` leaves everyone able to fight the
world but not each other, and `net.melee.damage` sets the blow.

Dying is where the engine stops and the ruleset starts. All the engine does is
mark the player dead, which replicates and takes them out of the target list;
what that means is a game's own answer. The platform ships the answer a session
gets when nobody has written a better one: `Respawns` waits five seconds and puts
them back on their feet at the spawn with a full pool. A ruleset sets
`Respawns.Delay`, or turns it off and handles `PlayerVitalsChanged` itself.

Two limits worth knowing. A listen host's own body has no health pool, so a
client cannot hurt the host; a dedicated server, where every player is a peer, is
complete. And a networked player has no actor record on the host, so its blows on
an NPC are attributed to the player form -- which is also why an NPC dying on the
host is not replicated yet, and a client sees it stop rather than fall. Everyone
also shares one spawn point, so a respawn can drop you next to whoever just
killed you.

### Configuring a server

A server outlives the shell that started it, so its settings live in a file
rather than in a wall of flags. `recreation-server` reads `server.cfg` beside the
working directory (or `--config <path>`):

```
# my server
name        Skyrim Together
port        29700
max_clients 48
data_dir    /games/Skyrim Special Edition/Data
mods_dir    ./server_mods
private

set net.bubble.radius 96

time 08:00
say the server is up
```

A value runs to the end of the line, so paths and names need no quoting.
`set` takes any of the engine's convars (the console's `convars` lists them) and
is applied before bring-up, so a convar a subsystem reads while starting still
takes effect. Every other line is a console command, run once the world is up.
Flags on the command line override the file, and `exec <path>` replays a file
into a running server.

### The server console

A dedicated server reads its terminal, so an operator can drive a live server
without restarting it or signalling it:

```
> status
name: Skyrim Together
port: 29700   players: 3/64
up: 4h 12m 8s
game time: 21:15
> say the server restarts in five minutes
> time 07:30
> set net.bubble.radius 96
> reload
```

The engine answers for what only it knows -- `status`, `reload` (re-scan the mods
directory and re-offer it to clients, the same thing `kill -HUP` asks for),
`time`, `weather`, `set` and `convars` over the engine's whole convar registry,
and `quit`. Every other line goes to the server's mods, where the platform's
command registry answers it (`players`, `say`, `kick`, `announce`, and whatever a
mod registered) and prints its reply on the same terminal. A console line runs as
the host operator, so it passes every permission check: whoever can type into the
server's terminal already owns the server.

`kick` disconnects for real: the transport says goodbye and that client drops on
the spot, whether or not it cooperates, and the server's roster clears when the
peer times out a few seconds later. Kicking somebody who is not connected says so
rather than reporting a kick that never happened.

### World sync

Everyone in a session stands in the same hour under the same sky. Both used to
be private: each machine started its clock when it finished loading and ran it
forward at the game's timescale, so two players who joined ten minutes apart
were hours apart in game time, one at noon and one after dark.

The host is now authoritative for both. It samples its clock and its weather
seed every tick and sends what a client cannot work out for itself: a changed
seed, a changed timescale, a clock it moved out from under the client's own
extrapolation (a script set the hour), and a slow heartbeat that mops up drift.
Ordinary passing time never travels -- the client runs its own clock at the
timescale it was told. A joining client is told the time as it is admitted, so
it loads into the session's hour rather than its own.

Weather itself never travels either. Selection is a pure function of (seed, game
time) over a climate both machines parsed from the same records, so the seed plus
the clock *is* the weather, cross-fades and lightning strikes included. The
message also names the weather in force on the host, which matters only where the
host's climate holds a weather the client's does not -- it resumed a savegame, or
a mod forced one -- and the client then aligns onto the same weather.

A host-side mod owns the shared world through the SDK:

```csharp
World.SetTime(7, 30);      // half past seven, for everyone
World.SetWeather(storm);   // a WTHR form; cross-fades in and evolves from there
```

Per-region weather stays per-player: the REGN area you stand in still overrides
the climate where you are, as it does in the games themselves.

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

`sdk/templates/mod` is a buildable starting point, and `sdk/README.md` covers the
managed API surface.

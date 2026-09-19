# Recreation SDK

The C# side of the engine, and what mods are written against: the gameplay API,
the multiplayer platform they build on, the host entrypoint, and the default
gamemodes that ship in the box. It lives at the top level instead of under
`engine/` because it's meant to be read and built against, not just compiled. (The
native bridge it talks to stays engine-side, in `engine/script/host/`.)

## What's in here

| Path | What |
|------|------|
| `Engine/`, `Modding/`, `Interop/` | the API you write mods against: `Game`/`Form`/`Actor`/`Quest`, `IMod`, `EventBus`, `GameBehaviour`, and the native marshalling |
| `Net/` and friends (`Teams/`, `Economy/`, `Chat/`, `Admin/`, ...) | the multiplayer platform (`Recreation.Net`) |
| `default_gamemodes/` | the Skyrim/Fallout/Starfield rulesets, shipped as optional content (see below) |
| `Samples/`, `templates/mod/` | example gamemodes, and a starter to copy out |
| `tests/` | the test runner (`dotnet run`, no test framework needed) |

It all compiles to one assembly, `Recreation.Scripting`, except the gamemodes,
which build on their own.

## Build and test

```sh
# dotnet only lives in the nix dev shell
./tools/build_managed.sh        # -> build/managed/Recreation.Scripting.dll
                                #  + build/managed/gamemodes/Recreation.{Skyrim,Fallout,Starfield}.dll
cd sdk/tests && dotnet run -c Release   # the test suite
```

## Writing a mod

Copy `templates/mod/` somewhere and build it. One rule matters: reference the SDK
**compile-time only** (the template already does, via `ExcludeAssets=runtime`).
That keeps `Recreation.Scripting.dll` out of your mod's output, so at runtime it
binds to the engine's copy instead of a private duplicate. Ship a duplicate and
the mod quietly fails to load.

```sh
dotnet pack -c Release sdk/Recreation.Scripting.csproj   # -> Recreation.Scripting.<ver>.nupkg
dotnet build -c Release                                  # in your mod folder
cp bin/Release/net9.0/MyMod.dll "$RECREATION_MODS_DIR/"
```

Pin a version and your mod keeps working until the next major SDK bump.

## Server-streamed client scripts

A mod does not have to be installed locally to run on a client. A server resource
(see the main README's "Multiplayer asset streaming") can list assemblies in its
`client_scripts.txt`; joining clients stream them, the player consents on the
loading screen (remembered per server; `net.stream_scripts` overrides), and the
engine loads them through `ModLoader.LoadStreamedScripts` after the streamed
content mounts. `[Realm]` filters apply unchanged: tag a streamed mod
`[Realm(ModRealm.Client)]` or `[Realm(ModRealm.Shared)]` and it runs on clients
exactly as a locally installed one would. Dependencies resolve from the same
streamed batch in any order, but ship every assembly the mod needs as its own
`.dll` — native libraries have no stream story. The runtime never unloads
assemblies, so new code the server pushes applies on the next join.

## Default gamemodes

The Skyrim/Fallout/Starfield rulesets each build as their own assembly and load at
boot from the `gamemodes/` folder beside `Recreation.Scripting.dll`. They behave
like built-in mods, but they're optional:

- drop a DLL from `gamemodes/` to remove that game,
- `RECREATION_NO_GAMEMODES=1` to skip them all,
- `RECREATION_GAMEMODES_DIR=<dir>` to load from elsewhere.

A ruleset only wakes up when its game is the one being played, so having all three
present costs nothing.

## Replicated vitals

A server-side mod sets a player's vitals; every client learns about them and the
engine puts them on the player's replica:

```csharp
// Server: announce and remember (unchanged values stay off the wire).
player.SetHealth(health, maxHealth, dead);

// Anywhere: react to the change for a player body on this machine.
EventBus.Subscribe<PlayerVitalsChanged>(e =>
{
    if (e.Dead) Hud.Notify("Someone fell", Value.Int(3));
});
```

The dead flag also drives the entity's `Dead` tag, so the render path holds a
downed pose. The engine combat system is not networked yet — today the producer
is mod code; when combat syncs, it feeds the same path.

## The shared world

`GameClock` reads the in-world time; `World` writes it, along with the sky. On a
host both are session-wide: the engine replicates the clock and the weather seed,
and every client adopts them, so one mod call moves the world for everyone.

```csharp
World.SetTime(7, 30);          // or World.SetTime(13.5f)
World.SetWeather(stormForm);   // a WTHR form; cross-fades in, then evolves on
```

Weather is not pinned: the requested weather comes in and the climate carries on
from there, so a server can stage a storm without freezing the sky. A client can
call these too, but the host's next world-state message puts the shared answer
back -- treat them as server-side.

## Versioning

One SemVer number in `Directory.Build.props`, surfaced as `SdkInfo.Version` and
logged at boot. New API is a minor bump; anything that breaks callers is a major
bump. A mod built against X.Y runs on any engine with SDK X.>=Y, and a major bump
is the only thing that breaks it.

(Two contracts hide in here: the C# API above, which mods care about, and the
native ABI in `engine/script/host/bridge.h`, which they never touch. The SDK and
engine ship together, so that one is always matched.)

## What's next

A version number is only as honest as the contract is clean, and right now
game-logic churn shares an assembly with the API. The open step is to split the
real contract (`Engine/`, `Modding/`, `Interop/`, plus the platform's public
surface) into its own `Recreation.Sdk` assembly, leaving the implementation behind
it.

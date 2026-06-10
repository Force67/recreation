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
| `engine/script` | scripting host, per game Papyrus adapters |
| `runtime` | entry point and main loop |
| `tools` | offline tooling |

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

SDL3, Vulkan and zlib are optional at build time. Without SDL3 the runtime is
headless (pass `-DRECREATION_FETCH_SDL3=ON` to download it), without Vulkan
the renderer compiles as a stub, without zlib compressed plugin records are
rejected at load time.

Targets: Windows, Linux, Android (via the NDK toolchain file).

## Mods

Mod compatibility follows the same rules the original games use. Plugins merge
records with last loaded winning, loose files override archives, and mount
order in the VFS decides priority. Papyrus is game specific and handled by per
game adapters in `engine/script`.

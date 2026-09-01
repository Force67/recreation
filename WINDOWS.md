# Windows

Two ways to get a Windows build. On a Windows machine, `scripts/setup-windows.ps1`
and the `windows` CMake preset build it with MSVC in the ordinary way. This
document is about the other one: **cross-building the real Windows binary from
Linux, and running it here**, which is how the port is developed and tested
without a second machine.

The toolchain and the Windows environment both come from
[winer](https://github.com/Force67/winer), a sibling checkout: llvm-mingw
(clang, UCRT target) produces the PE, and the Wine prefix winer builds runs it
against the host GPU.

```sh
scripts/build-windows.sh              # configure + build
scripts/build-windows.sh test         # ctest, every test a Windows PE under wine
scripts/build-windows.sh package      # assemble dist/win, a runnable tree
scripts/build-windows.sh run --menu   # build, then launch under wine
```

The shader and code generators (dxc, slangc, nanoc) stay host Linux binaries and
come from the nix dev shell, exactly as the native build gets them. rxpack is
the exception that needs saying: it packs the `.rxp` archives *during* the
build, so it has to be a host binary — the script configures a small native rx
tree to produce one and passes it as `RX_RXPACK`. Build width is derived from
free memory rather than core count: clang peaks around 1.5 GB on the heavier
translation units and this box has no swap.

`test` sets `CMAKE_CROSSCOMPILING_EMULATOR`, so ctest runs the actual Windows
executables through wine and the regression suite covers the port rather than
only the Linux build. USD scene loading is off here: tinyusdz builds itself with
`-fno-exceptions` and then throws on its own Windows path.

## What ends up beside the executable

`package` assembles the tree a Windows machine that has never seen this repo can
run. The layout is a contract with the runtime, which looks beside its own
executable for each of these before falling back to the paths CMake baked in
(those name build and source directories that only exist on the build machine):

| | |
|---|---|
| `shaders.rxp`, `rx_fonts.rxp` | compiled shader blobs and the engine's UI fonts |
| `screens/` | the `.ugui` HUD and menu fragments |
| `art/`, `vanilla/` | NEXUS key art, and screens translated out of the games' own Scaleform |
| `presets/` | the editable `.ini` render presets |
| `fonts/` | Roboto, the last-resort UI face |

`fonts/` is there because the interface loads a typeface by path. A machine with
no system font — a bare Wine prefix, a container, a stripped Windows install —
would otherwise render every label blank, which reads as a bug in the UI rather
than a missing file.

## Running it

```sh
cd dist/win
XRUN_DISPLAY=xvfb ../../../winer/scripts/xrun.sh ./recreation.exe --menu
```

`xrun.sh` supplies the three things a bare `wine recreation.exe` does not: wine's
own host libraries (so there is a Vulkan at all), wine's virtual desktop (SDL3's
win32 video driver reports *"No displays available"* without one) and visible
loader errors. See winer's README.

Game data takes a Windows path, and wine maps `/` to `Z:`:

```sh
./recreation.exe --data-dir 'Z:\path\to\Skyrim Special Edition\Data'
```

## C# scripting

The managed layer needs a .NET runtime the CLR host can load through hostfxr.
`winer/scripts/66-dotnet-core.sh` unzips one into the prefix and points
`DOTNET_ROOT` at it; the assemblies themselves are portable IL, built on the
host by the `managed_build` test fixture and copied into `dist/win/managed` by
`package`. Then:

```sh
RECREATION_SCRIPTING_DIR=managed ./recreation.exe --menu
```

## Backends

Both render. They are not equally complete.

**Vulkan** (`RX_RHI=vulkan`) is the full path: HUD, menus and the imgui debug
overlay all draw. On Windows that is a first-class target — every desktop
driver ships Vulkan — so this is the backend to use.

**D3D12** is the default when the backend is built. It renders the scene and
the imgui debug overlay; three things are still missing, and each says so in the
log:

- **No HUD or menus.** ultragui's backend (`runtime/ui/gui_backend.cc`) records
  raw Vulkan and bails on anything else, so the game interface is absent. The
  debug overlay used to be in the same position and is not any more: it draws
  through `engine/render/util/imgui_renderer.h`, the RHI backend, and an
  equivalent for ultragui is the work that would close this.
- **No mesh-shader or virtual-geometry passes.** Those shaders read buffers
  through device addresses (`vk::RawBufferLoad`), which has no DXIL spelling;
  they are on `RX_SHADER_NO_DXIL` and the engine falls back.
- **No upscaler.** FSR3/DLSS are Vulkan-only in this tree.

To test D3D12 here, install vkd3d-proton into the prefix and select it:

```sh
../winer/scripts/16-vkd3d-proton.sh
XRUN_D3D12=proton XRUN_DISPLAY=xvfb ../winer/scripts/xrun.sh ./recreation.exe --menu
```

Wine's own d3d12 is WineHQ vkd3d, which reports shader model 6.0, raytracing
tier 0 and mesh shader tier 0 — the renderer is rejected at device creation and
never draws. vkd3d-proton reports SM 6.6, DXR 1.1 and mesh shader tier 1.
`winer/scripts/46-gpu-probe.sh` prints exactly this, and is worth running before
blaming the renderer for anything.

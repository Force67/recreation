# What is left

Status of the Scaleform work, ordered so that each item is worth doing only
after the ones above it. Usage counts are measured over the 49 decompiled
Skyrim interface scripts, so the priorities are not guesses.

## Where it stands

The static half is done: the container, shapes, bitmaps, text, fonts and
timelines translate, and 49 Skyrim screens and 217 Fallout 4 screens come out as
markup plus assets. See `README.md` for how that works.

The dynamic half runs. `vm.{h,cc}` is an ActionScript 2 interpreter and
`stage.{h,cc}` builds a movie's display list as live objects, applies the classes
it binds with `Object.registerClass`, runs their constructors, plays their
timelines and dispatches their events. All 41 shipped Skyrim movies run without
crashing, hanging or exhausting the step budget. Skyrim's own
`StartMenu::setupMainMenu` executes and produces its option list from the game's
bytecode. `swfdump <movie> --run` reports what a movie built.

`runtime/ui/vanilla_runtime` binds that to the screen: 543 of the journal's 558
clips find the widget the same movie was translated into, and what the script
changes is written back to them. It is behind `RX_VANILLA_VM` and the
hand-written drivers still own the live menus.

## 1. Retire the hand-written drivers

The remaining arc. `vanilla_start_menu`, `vanilla_pause_menu` and
`GameUi::BuildFalloutMainMenu` reimplement in C++ what the movies' own code
already does, including panel-hiding that the timeline now resolves by itself.
They can go once the interpreter path is trusted on every screen, which needs:

- **Input into the movie.** `VanillaRuntime::Click` sends `onPress`/`onRelease`
  to the clip under the pointer; keyboard and pad still go to the drivers. The
  CLIK components listen through `gfx.ui.NavigationCode`, so a host that pushes
  those reaches every menu's navigation at once rather than per screen.
- **The host bridge answered.** `Vm::set_external_handler` is wired and nothing
  is installed on it. ~142 distinct native functions are called across the
  corpus; a menu asks for save lists, player info, settings values and platform
  through them, and returns nothing until they are answered.
- **A screen-by-screen check** that the interpreter path renders what the
  driver does, before the driver is deleted.

## 2. The menus themselves

Unchanged by the interpreter work, because nothing stands behind them yet.

- Pause menu: SAVE, LOAD, CONTROLS, HELP and INSTALLED CONTENT select but open
  nothing; the QUEST and STATS tabs do not switch.
- Start menu: LOAD, CREATIONS and CREDITS likewise.
- "Main Menu" from the quit list reopens the front screen with the world still
  loaded behind it, rather than tearing it down.
- No Skyrim wordmark. It is a 3D object in the main-menu scene
  (`meshes/interface/logo/logo01ae.nif`) and its texture is a UV atlas for that
  mesh, so it needs the NIF path, not the UI path.
- Fallout 4: the option list renders, but nothing stands behind the entries, and
  the sub-panels, the message-of-the-day body and the background art are unfilled.

## 3. ActionScript 3

Fallout 4 and Starfield are AVM2, which `abc.cc` disassembles but does not
execute, so none of the interpreter work reaches them. Their lists are filled
statically instead, by reading `listEntryClass` / `numListItems` out of the
bytecode (`ParseListBindings`). A real AVM2 interpreter is a second build of
comparable size to the AVM1 one.

## Not ours

Skyrim segfaults in world streaming (hair/actor) after a few minutes. It
reproduces with the vanilla UI switched off entirely, so it predates this work,
but it caps how long a session can be play-tested.

## Done

- The interpreter: values, objects, prototypes, closures, the DefineFunction2
  preloads, and the instruction set the menus use.
- The display list as live objects, with `Object.registerClass` applied.
- The timeline: `gotoAndStop` re-applies the frame, labels resolve,
  `_currentframe` / `_totalframes` follow, `play` steps and wraps.
- `addProperty` (1688 uses), the accessor the components are built on.
- Runtime clip creation: `attachMovie` (414), `getNextHighestDepth` (416),
  `removeMovieClip` (108), `createEmptyMovieClip`, `duplicateMovieClip`.
- Text fields with a real prototype, so the scripts' own `SetText` (659 uses)
  reaches them, plus `Stage.visibleRect` / `safeRect` and the listener globals.
- Events: `onLoad` after the constructor, `onEnterFrame` per tick, and dispatch
  by name for the rest.
- Timers: `setInterval` / `setTimeout` / `clearInterval`, drained by `Tick`.
- The host bridge, through `flash.external.ExternalInterface.call`.
- The Scaleform container tags. Of the twelve codes only `ExporterInfo` and
  `DefineSubImage` occur in any shipped movie; the sub-image layout was verified
  byte for byte, and the rest stay named and unparsed rather than guessed at.
- Binding the interpreter's clips to the translated widgets.

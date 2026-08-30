# What is left

Status of the Scaleform work, ordered so that each item is worth doing only
after the ones above it. Usage counts are measured over the 49 decompiled
Skyrim interface scripts, so the priorities are not guesses.

## Where it stands

The static half is done: the container, shapes, bitmaps, text, fonts and
timelines translate, and 49 Skyrim screens and 217 Fallout 4 screens come out as
markup plus assets. See `README.md` for how that works.

The dynamic half runs, and reaches the screen. `vm.{h,cc}` is an ActionScript 2
interpreter, `stage.{h,cc}` builds a movie's display list as live objects and
plays their timelines, and `bridge.{h,cc}` is the two-way channel a Bethesda
menu talks to its host through. All 49 shipped Skyrim movies run without
crashing, hanging or exhausting the step budget. `swfdump <movie> --run` opens a
movie the way the game does and reports what the player would have been shown.

`runtime/ui/vanilla_runtime` binds that to the widgets: 543 of the journal's 558
clips find the widget the same movie was translated into, and what the script
does to a clip (its text, its visibility, where it sits) is written back.

With `RX_VANILLA_VM=1` the Skyrim pause menu builds its own option list
(QUICKSAVE / SAVE / LOAD / INSTALLED CONTENT / SETTINGS / CONTROLS / HELP /
QUIT) out of its own bytecode, and the start menu builds NEW / LOAD / CREATIONS
/ CREDITS / QUIT out of `sendMenuProperties`. Neither goes through a driver.

## 1. Retire the hand-written drivers

`vanilla_start_menu`, `vanilla_pause_menu` and `GameUi::BuildFalloutMainMenu`
still own the live menus; the interpreter path runs instead of them under
`RX_VANILLA_VM`, not beside them. What the interpreter path still needs before
the drivers can go:

- **Input.** `VanillaRuntime::Click` sends `onPress`/`onRelease` to the clip
  under the pointer; keyboard and pad still go to the drivers. The CLIK
  components listen through `gfx.ui.NavigationCode`, so a host that pushes those
  reaches every menu's navigation at once rather than per screen.
- **The rest of the opening conversation.** `GameUi::OpenVanillaScreen` sends
  what makes each screen appear (`sendMenuProperties`, `RestoreSavedSettings`)
  and nothing else, so the tab labels still read "BUTTON TEXT" and the bottom
  bar still reads "Time, Date, Year". Those come from further host calls.
  `GameBridge::pending()` lists what a screen asked for and nobody answered,
  which is the work list.
- **Runtime-attached rows.** A list that builds its rows with `attachMovie` has
  no widget to bind them to; only the rows the timeline authored appear. The
  start menu's list is spaced by its script rather than by its markup, and comes
  out tighter than the original because of it.
- **A screen-by-screen check** before each driver is deleted.

## 2. The menus themselves

- Pause menu: the option list is live, but SAVE, LOAD, CONTROLS, HELP and
  INSTALLED CONTENT select without opening anything, and the QUEST and STATS
  tabs do not switch.
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
- `Array`: the constructor's own arguments (`new Array(a, b, c)` was building an
  empty object, so every fixed table a menu keeps came out empty), plus `slice`
  (230 uses), `splice` (195), `indexOf` (132), `concat` (31), `shift` (20) and
  `sort` (17).
- `InitArray` element order. It was reversed, which swapped every pair the
  scripts build, including GameDelegate's `[scope, method]`, so no message the
  game sent ever reached its handler.
- The display list as live objects, with `Object.registerClass` applied, and
  each clip carrying the box the frame gave it (`_x`, `_y`, `_width`,
  `_height`). A list divides its border's height by a row's to decide how many
  rows fit, so without those it showed nothing.
- Load events after the frame's own actions rather than during the build, which
  is the order the player uses. The root's actions install the helpers a menu's
  `onLoad` calls (`TextField.prototype.SetText` among them), so the other way
  round left every list filled with its placeholders.
- The timeline: `gotoAndStop` re-applies the frame, labels resolve,
  `_currentframe` / `_totalframes` follow, `play` steps and wraps. One shared
  display-list walk (`DisplayListAt`) now serves both the translation and the
  interpreter; the two had drifted, and the interpreter's copy was dropping the
  colour transform off a move, which left every fade on its first frame.
- `addProperty` (1688 uses), the accessor the components are built on.
- Runtime clip creation: `attachMovie` (414), `getNextHighestDepth` (416),
  `removeMovieClip` (108), `createEmptyMovieClip`, `duplicateMovieClip`.
- Text fields with a real prototype, so the scripts' own `SetText` (659 uses)
  reaches them, plus `Stage.visibleRect` / `safeRect` and the listener globals.
- Events: `onLoad`, `onEnterFrame` per tick, and dispatch by name for the rest.
- Timers: `setInterval` / `setTimeout` / `clearInterval`, drained by `Tick`.
- The host bridge, both ways: `GameDelegate.call` out through
  `ExternalInterface`, and `receiveCall` / `receiveResponse` in. `Open()` runs
  the `InitExtensions` the game runs, which is where a menu registers most of
  its callbacks (19 for the journal and the start menu, 27 for the HUD).
- The Scaleform container tags. Of the twelve codes only `ExporterInfo` and
  `DefineSubImage` occur in any shipped movie; the sub-image layout was verified
  byte for byte, and the rest stay named and unparsed rather than guessed at.
- Binding the interpreter's clips to the translated widgets, and writing back
  what the script changes: text (resolved through the interface's own "$KEY"
  table), visibility as opacity down the whole subtree (ugui does not inherit
  it), the timeline's own fades, and position when a script moves a clip off
  where its frame put it.

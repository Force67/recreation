# What is left

Status of the Scaleform work, ordered so that each item is worth doing only
after the ones above it. Usage counts are measured over the 49 decompiled
Skyrim interface scripts, so the priorities are not guesses.

## Where it stands

The static half is done: the container, shapes, bitmaps, text, fonts and
timelines translate, and 49 Skyrim screens and 217 Fallout 4 screens come out as
markup plus assets. See `README.md` for how that works.

The dynamic half runs and reaches the screen. `vm.{h,cc}` is an ActionScript 2
interpreter, `stage.{h,cc}` builds a movie's display list as live objects and
plays their timelines, and `bridge.{h,cc}` is the two-way channel a Bethesda
menu talks to its host through. All 49 shipped Skyrim movies run without
crashing, hanging or exhausting the step budget. `swfdump <movie> --run` opens a
movie the way the game does, ticks it, and reports what the player would see.

The Skyrim menus are drawn entirely by their own bytecode (`RX_VANILLA_VM=0`
turns the interpreter off and leaves the screens as the translation drew them),
in
the layout the designer authored: a centred list with the selection large and
bright and the rows falling away from it. The pause menu builds QUICKSAVE / SAVE
/ LOAD / INSTALLED CONTENT / SETTINGS / CONTROLS / HELP / QUIT and a bottom bar
reading `LEVEL 0` and `12:00am, 17th of Last Seed, 4E 201` from the world clock;
the start menu builds NEW / LOAD / CREATIONS / CREDITS / QUIT. Navigation goes
in through the components' own `handleInput`, so one host path drives every
screen's selection, activating a row opens the sub-panel the movie opens, and
what a row means reaches the host as the call the movie makes for it. The
hand-written Skyrim drivers are gone.

## 1. What is still missing on a Skyrim screen

- **The tab strip reads "BUTTON TEXT" some runs and QUESTS / GENERAL STATS in
  others.** The labels come from the authored `labelID`, and when they land they
  render, so this is an ordering race between a tab's construct handler and its
  class setter rather than a missing path. SystemTab is worse: its clip holds
  the placeholder even in `swfdump --run`, because the frame it is selected on
  rebuilds its text field after the label was applied.
- **The journal's QUIT row does not draw.** The clip holds `$QUIT`, is visible,
  sits at y=348 inside a 418-tall list, and its widget is written every frame.
  It is not `OnStage` hiding it (turning that off changes nothing) and there is
  no clipping mask around the list. The seven rows above it draw.
- **State groups under a clip nothing drives** keep whatever state they are
  showing, so a couple of tab chevrons overlap.
- **Runtime-attached rows.** A list that builds its rows with `attachMovie` has
  no widget to bind them to. Skyrim's lists place theirs on the timeline, so
  this has not bitten yet; Fallout 4's do not, which is what StampListRows
  stands in for.

## 2. Everything behind a row

The machinery is there: navigating, activating, opening a sub-panel and asking
the host for what a row means all work through the movies' own code. What is
missing is the host having anything to answer with.

- SAVE and LOAD ask for a save browser (`SAVE`, `UseCurrentCharacterFilter`,
  `onSaveLoadBatchComplete` and friends). recreation reads real .ess files
  already; nothing is wired to these.
- CONTROLS asks for the input map (`RequestInputMappings`, `SetButtonMapping`).
- The settings lists come up with their categories (Gameplay / Display / Audio
  render) but no values behind them.
- "Main Menu" from the quit list reopens the front screen with the world still
  loaded behind it. That one is not the UI's to fix: `SetupMainMenu` resolves
  universes and regenerates the menu backdrops on the GPU, which is init-time
  work that segfaults called from inside a frame, so the front screen is
  reopened rather than rebuilt. Tearing the world down needs a deferred
  teardown in the engine.
- No Skyrim wordmark. It is a 3D object in the main-menu scene
  (`meshes/interface/logo/logo01ae.nif`) and its texture is a UV atlas for that
  mesh, so it needs the NIF path, not the UI path.

`GameBridge::pending()` is the work list, and `swfdump --run` prints it.

## 3. Fallout 4 on the interpreter

`GameUi::BuildFalloutMainMenu` is the last hand-written driver. The interpreter
that would replace it now exists (§4) and produces the same option list from
Fallout 4's own code, so what is left is the wiring the Skyrim path already has:

- `VanillaRuntime` runs AVM1 only. It needs to pick the machine by what the
  movie carries, and bind AS3 display objects to widgets the way it binds clips.
- The host conversation is different. An AS3 menu takes the game's messages as
  plain methods on the instance (`Avm2::Invoke`) rather than through a delegate,
  and reaches back out through `Shared.BGSExternalInterface` rather than
  `gfx.io.GameDelegate`. Neither end is wired.
- Its sub-panels, the message-of-the-day body and the background art are
  unfilled.

## 4. ActionScript 3

The machine is in `avm2.{h,cc}`, with the structured decoder beside the
disassembler in `abc.{h,cc}`. It runs both AS3 corpora clean:

- **Fallout 4**: 2287 of 2287 classes construct, 218 movies fill a list, no
  opcode goes unimplemented and nothing hits the step budget. `mainmenu` builds
  $NEW / $LOAD / $SETTINGS / $CREW out of its own `InitList`, which is what
  `BuildFalloutMainMenu` hand-codes.
- **Starfield**: 18073 of 18278 classes construct on the same terms.

What it does not do yet:

- **205 Starfield classes do not construct.** They are the ones whose class
  object or constructor body does not resolve; nothing has looked at why.
- **No standard library beyond `Array.push`.** A menu that calls `String.split`
  or `Math.floor` gets undefined, which is silent. The opcode side is covered;
  the runtime side is not.
- **Types are not checked.** `coerce`, `astype` and `istype` pass values
  through, and `instanceof` answers false. Nothing in the menus turned out to
  branch on one, but a screen that does will take the wrong path quietly.
- **Exceptions.** `throw` ends the method and `newcatch` yields a bare object;
  there is no unwinding to a handler.
- **Display objects are stand-ins.** Reading a member a display object does not
  have yields another stand-in, because the player would have put one there.
  That is what lets `MainPanel_mc.List_mc.entryList` resolve before anything
  built it, and it means a genuinely absent member reads as an object rather
  than undefined.

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
- A bare assignment on a timeline names that timeline's own property rather than
  a global, which is how a placement's authored parameters reach the clip.
- The display list as live objects, with `Object.registerClass` applied, and
  each clip carrying the box the frame gave it (`_x`, `_y`, `_width`,
  `_height`). A list divides its border's height by a row's to decide how many
  rows fit, so without those it showed nothing.
- Load events after the frame's own actions rather than during the build, which
  is the order the player uses. The root's actions install the helpers a menu's
  `onLoad` calls (`TextField.prototype.SetText` among them), so the other way
  round left every list filled with its placeholders.
- The timeline: `gotoAndStop` re-applies the frame, labels resolve,
  `_currentframe` / `_totalframes` follow, `play` steps and wraps, and a frame's
  own actions run when a clip arrives (a fade that never runs its `stop()`
  plays past the end and wraps back to where it started). One shared
  display-list walk (`DisplayListAt`) serves both the translation and the
  interpreter; the two had drifted, and the interpreter's copy was dropping the
  colour transform off a move, which left every fade on its first frame.
- Retiring `vanilla_start_menu` and `vanilla_pause_menu`, 600 lines of C++ that
  reimplemented what the movies already say.
- Navigation, activation and the actions behind a row: a screen does not report
  which entry was picked, it asks the host for the thing that entry means
  (`StartNewGame`, `QuitToMainMenu`, `CloseMenu`), so acting on those is the
  whole of "the menu works". Each is guarded on the screen that asks for it
  being up, because a menu says these while it starts as well.
- Clip-event handlers off a PlaceObject (306 across the corpus, almost all
  `construct`), and a frame's own actions on arrival. On by default now that the
  translation carries the states they move a clip into; `RX_VANILLA_AUTHORED=0`
  turns them off, which is how to tell a problem in them from one elsewhere.
- `super()` climbing the chain. `Extends` writes the SUPERCLASS onto the
  subclass's prototype, so the constructor to call is the one recorded at the
  level whose body is running, and the next level up is where that class's own
  `super` starts. Resolving it from the instance gave every class in a chain the
  same answer, so a base constructor called itself until the depth limit stopped
  it: 387 calls cut out of one menu's start-up, among them the one that gives a
  button group the array it counts. Frames now size their registers to what a
  function declared rather than to the format's maximum, which is what made a
  deep chain expensive enough to need a tight limit in the first place.
- `SetPlatform`, sent on open. A screen that has not been told what it is
  running on lays its lists out for a controller and leaves most of the rows at
  zero alpha.
- Every state a clip can be in, translated as a sibling group per labelled frame
  that puts something different on the display list, which a host shows by
  matching `_currentframe`. Both halves of that rule earn their keep: counting
  unlabelled keyframes as states takes the journal from 1.4k widgets to 61k, and
  counting a fader's labelled tween frames (which place the same thing all the
  way through) triples the whole menu under every fader. As it stands the
  journal goes from 869 widgets to 1607 and carries 289 states.
- `Try`/`Catch`/`Finally`. The three blocks sit inline after the action, so a
  machine that falls through runs all three; the catch block stores an exception
  nobody threw and the stack is out of step for the rest of the function.
- `SomeClass(value)` as a cast rather than a construction, which is how the
  journal gets its tab strip.
- `addProperty` (1688 uses), the accessor the components are built on.
- Runtime clip creation: `attachMovie` (414), `getNextHighestDepth` (416),
  `removeMovieClip` (108), `createEmptyMovieClip`, `duplicateMovieClip`.
- Text fields with a real prototype, so the scripts' own `SetText` (659 uses)
  reaches them, plus `Stage.visibleRect` / `safeRect` and the listener globals.
- Events: `onLoad`, `onEnterFrame` per tick, and dispatch by name for the rest.
- Timers: `setInterval` / `setTimeout` / `clearInterval`, drained by `Tick`.
- The host bridge, both ways. `GameDelegate.call` goes out through
  `ExternalInterface`; `receiveCall` brings a message in; `Open()` runs the
  `InitExtensions` the game runs, which is where a menu registers most of its
  callbacks (19 for the journal and the start menu, 27 for the HUD). An answer
  has to be given from inside the call the movie is making
  (`GameBridge::set_answerer`): GameDelegate drops its response slot the instant
  that call returns, so a host replying a frame later replies to nothing.
- Navigation in through the components' own `handleInput`, with the `details`
  the game builds.
- The Scaleform container tags. Of the twelve codes only `ExporterInfo` and
  `DefineSubImage` occur in any shipped movie; the sub-image layout was verified
  byte for byte, and the rest stay named and unparsed rather than guessed at.
- Binding the interpreter's clips to the translated widgets, and writing back
  what the script changes: text (resolved through the interface's own "$KEY"
  table), visibility as opacity down the whole subtree (ugui does not inherit
  it), the timeline's own fades, and position when a script moves a clip off
  where its frame put it.

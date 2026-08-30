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

With `RX_VANILLA_VM=1` the Skyrim pause menu is drawn entirely by its own
bytecode: the eight options, the selection, "recreation" as the version text,
and a bottom bar reading `LEVEL 0` and `12:00am, 17th of Last Seed, 4E 201` from
the world clock. The start menu builds NEW / LOAD / CREATIONS / CREDITS / QUIT
the same way. Navigation goes in through the components' own `handleInput`, so
one host path drives every screen's selection.

## 1. Finish the CLIK component chain

The translation carries every state now (see Done), so the export is no longer
what limits a screen. What is left is one chain inside the shipped component
library, and it is the last thing between here and
`Stage::set_authored_state(true)` in the runtime.

Symptom: the journal's tab strip comes back empty. `Quest_Journal.onLoad` does
`ButtonGroup(this.QuestsTab.group).length`, that comes back `undefined`, and the
NaN travels into `iCurrentTab`, so `SwitchPageToFront` never runs and the System
page is left on its hidden frame. With the authored state off the page is shown
anyway and the menu works; with it on the page goes with the tab strip.

What is known about it:
- The tab does get its authored `inspectableGroupName = "tabGroup"`, and a
  ButtonGroup for it is created on the parent (`_buttonGroup_tabGroup` exists).
- But the group the tab ends up holding is the DEFAULT one, named
  "buttonGroup", so something calls `__set__group` again afterwards.
- That instance has `name` set and `children` unset, two adjacent statements in
  `ButtonGroup`'s own constructor. Its body stops between them, which is the
  thing to find; `__get__length` returns `this.children.length`, hence the
  `undefined`.
- Two earlier stops of the same shape turned out to be the interpreter running a
  `try`'s catch block as well as its body (fixed), so look for another construct
  whose inline layout the machine walks straight through.

## 2. Retire the hand-written drivers

`vanilla_start_menu`, `vanilla_pause_menu` and `GameUi::BuildFalloutMainMenu`
still own the live menus; the interpreter path runs instead of them under
`RX_VANILLA_VM`, not beside them. Left before they can go:

- **Answering more of the conversation.** `GameUi::AnswerVanillaCall` answers
  the player info, the version text and two feature flags. `PlaySound`,
  `myLog`, `RememberCurrentTabIndex` and `SetSaveDisabled` are deliberately
  unanswered; whatever else a screen waits on collects in
  `GameBridge::pending()`, which `swfdump --run` prints.
- **A screen-by-screen check** before each driver is deleted. Only the journal
  and the start menu have been looked at on screen.
- **State groups under a clip nothing drives.** A widget reached without a clip
  keeps whatever state it is showing, so a couple of tab chevrons draw over
  each other. Harmless, and it goes when §1 does.

## 3. The menus themselves

- Pause menu: the option list is live and navigable, but SAVE, LOAD, CONTROLS,
  HELP and INSTALLED CONTENT select without opening anything, and the QUEST and
  STATS tabs do not switch. The widgets for those states are translated now, so
  what is left is §1.
- Start menu: LOAD, CREATIONS and CREDITS likewise.
- "Main Menu" from the quit list reopens the front screen with the world still
  loaded behind it, rather than tearing it down.
- No Skyrim wordmark. It is a 3D object in the main-menu scene
  (`meshes/interface/logo/logo01ae.nif`) and its texture is a UV atlas for that
  mesh, so it needs the NIF path, not the UI path.
- Fallout 4: the option list renders, but nothing stands behind the entries, and
  the sub-panels, the message-of-the-day body and the background art are unfilled.

## 4. ActionScript 3

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
- Clip-event handlers off a PlaceObject (306 across the corpus, almost all
  `construct`), behind `set_authored_state` until §1 is done.
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

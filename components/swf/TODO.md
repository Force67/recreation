# What is left

Status of the Scaleform work, ordered so that each item is worth doing only
after the ones above it. Usage counts are measured over the 49 decompiled
Skyrim interface scripts, so the priorities are not guesses.

## Where it stands

The static half is done: the container, shapes, bitmaps, text, fonts and
timelines translate, and 49 Skyrim screens and 217 Fallout 4 screens come out as
markup plus assets. See `README.md` for how that works.

The dynamic half now exists too. `vm.{h,cc}` is an ActionScript 2 interpreter
and `stage.{h,cc}` builds a movie's display list as live objects, applies the
classes it binds with `Object.registerClass`, runs their constructors and plays
their timelines. All 41 shipped Skyrim movies run without crashing, hanging or
exhausting the step budget. Skyrim's own `StartMenu::setupMainMenu` executes and
produces its option list from the game's bytecode. `swfdump <movie> --run`
reports what a movie built.

What none of that does yet is reach the screen.

## 1. Bind clips to widgets

The blocker, and the only item that changes what a player sees. `Stage` builds
556 clips for the journal, classes 143 of them and plays 205 stateful
timelines - all as interpreter objects with no connection to the widget tree the
exporter emitted. The screen is still drawn from the static translation and
driven by hand-written C++.

Everything below is machinery without an outlet until this exists. Once it does,
`runtime/ui/vanilla_start_menu`, `vanilla_pause_menu` and
`GameUi::BuildFalloutMainMenu` can go, including all of their panel-hiding: the
timeline already resolves those states properly, which is what the hiding was
standing in for.

## 2. Interpreter gaps

Ordered by how much the shipped scripts actually need them.

| Missing | Uses in the corpus | Note |
|---|---|---|
| `addProperty` | 1688 | getters/setters; the biggest hole |
| `getNextHighestDepth` | 416 | paired with `attachMovie` |
| `attachMovie` | 414 | runtime clip creation |
| `.text =` on fields | 432 | the text-field object is minimal |
| `setInterval` | 211 | currently a stub returning 0 |
| `onEnterFrame` | 210 | see event dispatch below |
| `onPress` | 188 | |
| `removeMovieClip` | 108 | |
| `onRollOver` | 89 | |
| `createEmptyMovieClip` | 13 | |
| `duplicateMovieClip` | 6 | |

**`addProperty` is the one to do first.** Without it the property form of an
accessor reads `undefined`, which is why the code here calls `__get__entryList()`
directly rather than reading `list.entryList`. Any script that uses the property
form silently gets nothing.

**Event dispatch** is the other structural gap: 113 `onLoad`, 86 `onMouseWheel`,
47 `onEnterFrame`, 37 `onKeyDown` handlers are assigned and never fire. It is
why only 4 of the ~142 host-bridge functions are ever called - the rest sit
behind interaction. The bridge itself is wired
(`flash.external.ExternalInterface.call`, `Vm::set_external_handler`); it has
nothing to deliver.

**`play()` / `stop()`** are no-ops for want of a frame ticker. Every state a menu
*shows* is reached by an explicit `gotoAndStop`, so this costs only the tween
between them.

## 3. The menus themselves

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

## 4. Owed

Two things named as in scope and not delivered.

**The GFX container tags.** All twelve Scaleform tag codes (1000-1011) are named
in `swf.h` and parsed by nothing. `quest_journal.gfx` carries one
`GFXExporterInfo` and 163 `GFXDefineSubImage` tags, which move its bitmaps out to
an external atlas. It costs nothing today only because Bethesda ships the `.swf`
twin with everything embedded and the translation prefers it; a movie shipped
only as `.gfx` would lose all of its raster art.

**ActionScript 3.** Fallout 4 and Starfield are AVM2, which `abc.cc` disassembles
but does not execute, so none of the interpreter work reaches them. Their lists
are filled statically instead, by reading `listEntryClass` / `numListItems` out
of the bytecode (`ParseListBindings`). A real AVM2 interpreter is a second build
of comparable size.

## Not ours

Skyrim segfaults in world streaming (hair/actor) after a few minutes. It
reproduces with the vanilla UI switched off entirely, so it predates this work,
but it caps how long a session can be play-tested.

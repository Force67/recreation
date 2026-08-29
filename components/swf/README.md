# swf

Hides **Flash and Scaleform GFx**, the format every Bethesda game since Oblivion
builds its whole interface in. Nothing above this component parses a SWF tag.

- Container: `swf` opens `.swf` (FWS/CWS) and `.gfx` (FFX/GFX/CFX), decompresses
  it and splits the tag stream. `types` carries twips, matrices, colour
  transforms and the bit reader every record is built from.
- Art and content: `shape` (DefineShape 1-4, including the edge-soup to contour
  stitching that turns left/right fill styles into real outlines), `bitmap`
  (lossless and JPEG characters, plus PNG export), `text` (edit text fields,
  static text, fonts and their code tables).
- Structure: `movie` builds the character dictionary and every timeline, so a
  display list can be walked with names, matrices, masks and 9-slice grids
  attached.
- Logic: `avm1` and `decompile` lift ActionScript 2 bytecode back to source;
  `abc` reads the ActionScript 3 (AVM2) blocks the newer games use.
- Output: `svg_export` writes a shape as an SVG document, `font_export` converts
  an embedded typeface to TrueType, and `ugui_export` translates a whole movie
  into libultragui markup plus its assets.

## Which bytecode a game uses

Skyrim's menus are ActionScript 2: 1337 `DoInitAction` blocks across the shipped
`Skyrim - Interface.bsa`, and not one `DoABC`. Fallout 4 and Starfield moved to
ActionScript 3, where the classes live in `DoABC` and `SymbolClass` replaces
`ExportAssets` as the character-to-name binding.

So `decompile` (AS2) recovers running source, while `abc` (AS3) recovers the
full symbol structure - packages, classes, inheritance, members with their real
names and types - and disassembles the method bodies. The visual half of the
pipeline is the same tag set in both, so shapes, bitmaps, text and timelines
translate identically for every game.

## Translating a movie to ugui

`ExportUgui` walks the display list and maps Flash's model onto libultragui's:

| Scaleform | libultragui |
| --- | --- |
| DefineSprite | nested `panel`, named by its instance name |
| PlaceObject matrix | absolute `left`/`top`/`width`/`height` (+ `rotation`) |
| solid-filled rectangle | `panel` with a `background` colour |
| gradient-filled rectangle | `background` / `background-end` / `gradient-angle` |
| other vector art | `image` bound to an exported SVG |
| imported bitmap | `image` bound to an exported PNG |
| DefineEditText | `text`, keeping its ActionScript variable as a note |
| clip-depth mask | `panel` with `overflow: hidden` wrapping the depths it covers |
| colour transform | folded down the tree into leaf colours and `opacity` |

### What a menu needs besides its own tags

A shipped menu is not self-contained, and a translation that ignores that comes
out as an empty shell:

- **Its text is keys.** Fields hold `$LEVEL`, `$Saving...`; the real strings live
  in `interface/translate_<language>.txt` (`bethesda::InterfaceStrings`).
- **Its typeface is embedded.** A field names a symbol like
  `$EverywhereMediumFont`; `interface/fontconfig.txt`
  (`bethesda::InterfaceFontConfig`) maps that onto a family inside one of the
  font movies, whose glyphs `font_export` turns into a TrueType file.
- **Its formatting is HTML.** A field's real face, size, colour, letter spacing
  and alignment sit in the `<p align><font face size color letterSpacing>` markup
  of its own value, not in the DefineEditText tag.
- **It is several movies.** An inventory screen is a frame that imports its
  lists, item card and button bar; following those imports is the difference
  between fifteen widgets and three hundred.
- **Some of it is never drawn.** Flash marks a hit area or a component's state
  swatch by outlining it with a stroke at an alpha of a couple of 255ths, and
  Bethesda leaves a developer overlay in a few menus under an instance whose
  name says `Debug`. Both are skipped.

### Two coordinate traps

The exporter writes the movie's own stage coordinates and lets the host scale
the stage to the viewport, the way Scaleform does; the manifest leads with
`!stage <w> <h>` so the host knows what to scale from. Baking a scale in instead
pins the screen to a corner at a fixed size.

Every length carries an explicit `px`. ugui reads a bare decimal between 0 and 1
as a flex fraction rather than pixels, and a movie's coordinates land in that
range constantly (a half-pixel nudge on a list row); without the unit those
widgets silently collapse and whole columns of a menu go missing.

Two more details are worth knowing. Colour transforms are concatenated on the way
down rather than emitted per node, because ugui's `opacity` does not inherit and
Flash's does; without that, every plate a menu fades in would draw at full
strength. And a Flash mask clips to its shape while ugui clips to a box - every
mask in the shipped menus is a rectangle, so the two agree, but a shaped mask
would clip wider than the original.

What cannot come across is anything the movie only knows at runtime: list rows
the game fills in, tweened positions, meter fills the code scales, and masks
installed from ActionScript rather than by clip depth. The translation is the
movie's opening frame, which for a menu is its fade-in state - mostly empty,
because Scaleform ships a menu transparent and its own code fades it in.
`reveal_faded` shows what the movie is authored to look like instead, at the
cost of stacking every page of a multi-page screen on top of each other.

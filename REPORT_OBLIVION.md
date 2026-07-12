# Oblivion support (feature/oblivion-support)

Classic Oblivion (NOT Remastered: BSA v103, TES4 HEDR 1.0, Gamebryo 20.0.0.x
NIFs) loads and renders its Tamriel worldspace, both as the primary game
(`--game oblivion`) and as an `--add-game` secondary domain beside Skyrim.

## What was wrong

Four blockers, fixed in dependency order (commit 5b9a772):

1. **No game registration.** No `Game::kOblivion`, no profile, no detection.
2. **BSA v103 rejected.** The reader gated on v104/v105, and the 0x100 archive
   flag was treated as "embedded filenames", which is only true from v104 on.
3. **20-byte record/group headers.** TES4-era plugins use 20-byte headers, not
   the 24-byte Skyrim/FO4 layout, so every record read was misaligned.
4. **No Gamebryo NIF reader.** Oblivion NIFs (10.0.1.2, 10.1.x, 10.2,
   20.0.0.4/5) have no block-size table, so the modern reader cannot skip
   unknown blocks; every block type in a file must be parsed sequentially.

Plus smaller gaps: Oblivion LTEX has no TNAM/TXST (terrain splat had no
texture source), WRLD carries no DNAM (no default water height, so Lake
Rumare and the coast were dry), and no sensible default start cell.

## What changed

All on `feature/oblivion-support`:

- `engine/bethesda/game_profile.{h,cc}`: `Game::kOblivion` profile (BSA,
  HEDR 1.0, Oblivion.esm, exterior "Tamriel", no localized strings,
  `record_header_size = 20`) + data-dir autodetection.
- `engine/bethesda/plugin.{h,cc}`: profile-driven 20 vs 24 byte record/group
  header reads. Downstream record consumers were already layout-agnostic;
  1.17M records index correctly.
- `engine/bethesda/bsa.cc`: accept v103; embed-filenames flag gated to v>=104.
- `engine/bethesda/nif_gamebryo.cc` (NEW, ~1150 lines): sequential classic
  Gamebryo reader. ~55 block types walkable: NiTriShape/NiTriStrips (with
  unstripping), inline NiTexturing/NiMaterial/NiAlpha/NiStencil properties
  inherited down the node tree, tangents from NiBinaryExtraData, structural
  walks of bhk Havok collision, constraints, controllers, lights. Prototyped
  against the real BSAs with an exact-byte-consumption oracle (2926/3000
  world meshes, then 19/23 of the 10.0.1.2 stragglers = groundcover grass).
- `engine/bethesda/nif.{h,cc}`: version dispatch at the top of
  `ConvertNifScene`, `NifConversion.gamebryo` flag.
- `engine/bethesda/converters.cc`: `_n.dds` normal-map convention for
  gamebryo conversions, existence-checked via the Vfs.
- `engine/world/land_baker.cc`: LTEX ICON fallback for the terrain splat
  (`textures/landscape/<icon>`, `_n` derivation for the normal slot).
- `engine/world/cell_streaming.{h,cc}` + `runtime/content_load.cc`: fallback
  water height 0 when the WRLD has no DNAM; Weye (3,10) default start cell.
- `runtime/main.cc`, `tools/esminfo/main.cc` (finalize commit): `--game`
  parse + help text and esminfo game-id mapping list oblivion (and the
  previously missing starfield).

## Verification (finalize session, full rebuild first)

Build: clean configure + build, exit 0. Tests: **75/75 ctests pass**.
`esminfo Oblivion.esm oblivion` parses the plugin (1,252,095 records in the
header, per-type counts print correctly).

Screenshots (RX_SCREENSHOT at t=25s, pinned noon/pleasant/timescale 0,
streaming idle before capture):

- `build/shots-oblivion/oblivion_weye.png`: default `--game oblivion` run.
  Weye village: textured timber-framed houses, stone chapel, mossy granite
  rocks, LTEX-splatted terrain, Lake Rumare behind. 49 cells, 1737 entities,
  44 water planes, 23.7k grass instances streamed to idle.
- `build/shots-oblivion/oblivion_lake_rumare.png`: fixed cam looking east
  from Weye. Lake Rumare water planes render, the Imperial City bridge spans
  the lake, Ayleid ruin and green shoreline terrain visible.
- `build/shots-oblivion/skyrim_regression.png`: known Whiterun vantage
  (`RX_CAM="321.7,60,400,0,-0.38"`), unchanged: city, tundra, distant-LOD
  mountains all normal.

Per-run conversion failures are ~15 files: particle fires/smoke, butterflies
(NiPathInterpolator), markers, a few skinned shapes. All fail-and-skip.

## Remaining gaps / next steps

1. **SpeedTree `.spt` trees.** Oblivion trees are not NIFs; forests currently
   lack canopies. Needs an .spt parser (or a billboard fallback from the
   tree billboards in the BSAs).
2. **Particle systems** (fires, smoke, butterflies) and **skinned meshes**
   fail-and-skip in the Gamebryo reader. Skinning needs NiSkinInstance/
   NiSkinData conversion into the existing skinned-mesh path.
3. **No distant LOD.** Oblivion's LOD format (landscape LOD quads + distant
   tree billboards) differs from the .btr/.bto pipeline; the horizon
   currently ends at the streaming radius (visible as open sea beyond Lake
   Rumare).
4. **Terrain color A/B.** Near the IC the splat reads sandier than the real
   game; needs a side-by-side against ground truth (possibly the ICON
   fallback picks a different layer order than the engine's blend).
5. **Dialogue/quests/scripts.** Oblivion uses obscript, not Papyrus;
   completely untouched.
6. Interior cells, creatures/NPCs (skeleton.nif is Gamebryo-skinned), and
   Shivering Isles content are unverified.

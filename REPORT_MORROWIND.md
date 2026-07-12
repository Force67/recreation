# Morrowind support (feature/morrowind-support)

Classic Morrowind (TES3, 2002: BSA v0x100, flat 16-byte records, NetImmerse
4.0.0.2 NIFs) loads and renders its implicit Vvardenfell worldspace, built on
top of the Oblivion scaffolding rather than duplicating it.

## What was wrong

Morrowind predates every layout the Bethesda pipeline assumed. Blockers, fixed
in dependency order (commit 00b601a):

1. **No game registration.** No `Game::kMorrowind`, no profile, no detection.
2. **BSA v0x100 rejected.** The magic check rejects the old archive. TES3 BSA
   is the pre-`BSA\0` hash-table layout (version 0x100): a flat file table with
   name offsets, no magic tag, unlike the v10x format.
3. **Flat 16-byte TES3 records with no GRUP groups.** TES3 records are flat
   (4CC name + u32 size + u32 unknown + u32 flags, then NAME+size subrecords),
   the first record is `TES3`, and there are no GRUP groups at all. Object ids
   are strings, not the 32-bit form ids the rest of the engine keys on.
4. **NetImmerse 4.0.0.2 NIFs below the reader's range.** The Gamebryo reader
   covers 10.x..20.0.0.x; 4.0.0.2 is below that range, so no world mesh loaded.
5. **65x65 / 8192-unit LAND vs the 33x33 / 4096-unit pipeline.** TES3 exterior
   cells are 8192 units and carry a 65x65 LAND grid, but the streamer and
   land-baker expect 4096-unit cells with 33x33 heightfields.

## What changed

All on `feature/morrowind-support` (commit 00b601a):

- `engine/bethesda/game_profile.{h,cc}`: `Game::kMorrowind` profile with the
  `flat_tes3` record layout + Morrowind.bsa / Morrowind.esm data-dir
  autodetection. One implicit exterior worldspace (synthesized Vvardenfell
  WRLD), no localized strings.
- `engine/bethesda/tes3.{h,cc}` (NEW): TES3 -> modern record translator. Flat
  16-byte records with string ids become synthesized Skyrim-shaped records at
  load (synthetic form ids, inline FRMR cell refs binned by position into
  REFR). Each 8192-unit exterior cell splits 2x2 into virtual 4096-unit cells
  so the 65x65 LAND re-encodes exactly into four 33x33 VHGT/VNML/VCLR
  quadrants and the streamer/land-baker run unchanged. The VTEX 16x16 texture
  grid (stored in 4x4 chunks) is deswizzled into BTXT/ATXT+VTXT over
  synthesized LTEX/TXST pairs naming the real `Tx_*.dds` files.
- `engine/bethesda/bsa.cc`: `LegacyBsaProvider` for the v0x100 archive (flat
  file table + name offsets, no magic tag).
- `engine/bethesda/nif_gamebryo.cc`: 4.0.0.2 path. Inline-typed blocks without
  a type table, u32 bools, single chained extra-data ref, velocity /
  bounding-volume AVObject tail, 4.x NiTexturingProperty / NiSourceTexture /
  NiTextureEffect / particle / keyframe / skin layouts, `.tga`/`.bmp` refs
  swapped to the `.dds` the BSA ships. Includes the NiDynamicEffect
  affected-node list and the byte clipping-plane fix.
- `runtime/content_load.cc`: sea-level 0 fallback water, Seyda Neen default
  start cell.
- `tools/esminfo/main.cc` (assetdump): `--nifscan` coverage oracle.

## Verification (full rebuild first)

Build: clean configure + build, exit 0. Tests: **75/75 ctests pass**.

TES3 translation log line: 48,296 source records -> 156,034 synthesized
records, 137,166 refs.

`--nifscan` oracle: 3728/3818 referenced world models convert. The 90 fails
are magic VFX, particles, and skinned banners.

Screenshots (RX_SCREENSHOT, pinned pose, streaming idle before capture) at
`build/shots-morrowind/`:

- Bitter Coast: swamp terrain and shoreline, VTEX splat over the real
  `Tx_*.dds` land textures.
- Seyda Neen: the arrival ship, docks, and shacks render.
- Balmora: Hlaalu adobe housing plus the Temple.
- Oblivion Weye regression: identical to the Oblivion report vantage,
  unchanged.

## Remaining gaps / next steps

1. **Tribunal / Bloodmoon cross-plugin string ids.** The synthetic-form-id
   translator resolves ids within a single plugin; cross-plugin string
   references across the expansions are unhandled.
2. **Interiors.** Interior cells are unverified.
3. **NPCs / creatures + skinned 4.x meshes.** 4.0.0.2 NiSkinInstance/NiSkinData
   skinning is not converted into the skinned-mesh path.
4. **Banners / particle VFX.** The 90 nifscan fails (magic effects, particles,
   skinned banners) fail-and-skip.
5. **Dialogue / obscript.** Morrowind script and dialogue are untouched.
6. **No distant LOD.** The horizon ends at the streaming radius.
7. The bluish sail-like plane at Seyda Neen (a mesh reading as an oversized
   flat quad) is unresolved.

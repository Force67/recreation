# Starfield procedural-planet surface generation — MVP first slice

Branch: `feature/starfield-procgen` (off `main`, recreation). rx is **untouched**.

This lands a coherent first slice: **one real Starfield planet's biome map decoded
from the shipped data, driving a bounded procedural landing tile with
biome-appropriate ground + scatter, rendered and screenshot-verified.** It is NOT
full Starfield parity; the honest gaps are at the end.

## What it does (end to end)

`RX_STARFIELD_PLANET="zeta ophiuchi ii"` boots a Starfield data dir into a
synthesized procedural tile instead of the authored New Atlantis worldspace:

1. Reads `planetdata/biomemaps/zeta ophiuchi ii.biom` from the Starfield BA2s via the Vfs.
2. Decodes it: 1 biome, dominant `0xc5719`.
3. Resolves that biome record: `BIOM 0xc5719 = CrateredNoLife09`, surface `Cratered`,
   first material layer `LNAM -> LTEX LDirtPacked09_GrayRocks -> Materials\Terrain\DirtPacked09_GrayRocks.mat`,
   which the CDB material db resolves to the **real ground texture**
   `textures/landscape/ground/dirt/dirtpacked09_grayrocks_color.dds`.
4. Generates a 5x5-cell (`radius=2`) tile of engine-native FBM value-noise heightfields,
   deterministic from the planet name, realized as terrain meshes + heightfield colliders
   through the same asset/mesh/renderable path the LAND streamer uses.
5. Scatters 300 procedural boulders (deterministic per-tile seed) tinted by the biome.
6. Places the camera on the surface and streams nothing further (a bounded tile).

A 5-biome planet (`RX_STARFIELD_PLANET="zelazny v"`) resolves five distinct real
grounds — Canyons->DirtPacked02_Pale, Frozen->**SnowSmooth01**, Hills->DirtPacked07_Gray,
Mountains->DirtPacked02Rough, Desert->**SandPacked04** — proving the biome->ground chain
works across dirt/snow/sand, and the biome-map sampler picks different ground per area.

## Formats / records decoded

- **`.biom` biome map** (`engine/bethesda/biom.{h,cc}`) — byte-exact decoder ported
  from the verified spec. `u16 magic=0x0105`, `u32 numBiomes`, `u32[] BIOM FormIDs`,
  then two hemispheres, each a 256x256 `u32` biome-FormID grid + a 256x256 `u8`
  resource overlay. **Correction to the prior spec:** region 0 has a 16-byte header
  (`numGrids, w, h, n`); region 1 has a **12-byte** header (`w, h, n`, NO leading
  numGrids). Both sample files parse to 0 trailing bytes. Unit-tested (`biomtest`, 13 checks).
- **`BIOM` record** (`engine/bethesda/planet.{h,cc}`, `ResolveBiomeGround`) — reads
  `EDID`, `SNAM` (surface archetype), `BMC1` (map colour -> fallback ground tint), and
  the `LNAM` material-layer blocks (LTEX FormID at offset 4). These fields are plain
  subrecords, so `Record::Find`/subrecord iteration is enough; the record's BFCB/BFCE
  `_Component` wrappers carry only metadata I don't need, so no component-block walker
  was required for the MVP.
- **`LTEX` -> `.mat` -> textures** — the biome's first LTEX's `BNAM` (`.mat` path) is
  resolved through a `StarfieldMaterialDb` (the same `materials/materialsbeta.cdb`
  reader the NIF converters use) to real diffuse/normal DDS paths.
- **PNDT/STDT** — NOT parsed. The `.biom` filename IS the planet identifier the user
  passes, and the biome ids inside it fully drive the ground; the PNDT would only add
  radius/gravity/parent-star metadata the surface MVP doesn't consume. Noted as a gap.

## Tile-gen pipeline (`engine/world/planet_tile.{h,cc}`)

`PlanetTile::Generate(world)`:
- **Heightfield**: `HeightBethesda(bx,by)` = 4-octave FBM value noise (hash-lattice,
  smootherstep), base wavelength ~0.9 cell, `height_scale=320` units (~4.6 m relief).
  4 octaves keeps the finest lattice above the vertex-spacing Nyquist limit (the
  original 5-octave/over-scaled version aliased into vertical spikes — fixed). The same
  function feeds the mesh, the colliders, and the scatter, so they always agree.
- **Terrain mesh**: a 33x33 grid per cell (identical layout to LAND `SpawnTerrain`),
  with heightfield-derived vertex normals so lighting reads the relief, uploaded via
  the renderer, spawned as `world::Transform + world::Renderable` (the components the
  frame loop's render sweep reads), with a Jolt heightfield collider.
- **Ground material**: `GroundMaterial(biome_index)` binds the resolved diffuse/normal
  (real Starfield textures) with the `BMC1` colour as the base-color factor / fallback
  tint when the CDB doesn't resolve. `RX_PLANET_TEXTURE=0` drops the texture to isolate
  shape from texture (a diagnostic).
- **Scatter**: `SpawnScatter` places procedural low-poly boulders (jittered UV-spheres,
  deterministic per cell seed), sat on the heightfield.

Coordinate convention matches CellStreamer exactly: mesh stays in Bethesda object
space (Z-up), each entity carries `engine = (x, z, -y) * units_to_meters` + the -90-deg-about-X
rotation, `scale = units_to_meters`.

## Entry point / wiring

- `RX_STARFIELD_PLANET=<.biom stem>` (`runtime/content_load.cc`, `LoadPlanetTile`) —
  when set and the game is Starfield, boots the tile instead of `SelectWorldspace`.
  The primary CellStreamer is still constructed but never selects a worldspace, so its
  `Update` returns early (no interference). Camera is set from the generated ground.
- Env registration follows the existing `base::Option` pattern.

## What renders (screenshots)

All at 1920x1080 via `RX_SCREENSHOT`. `zeta ophiuchi ii`, fixed camera, pinned time/weather.

- `<scratchpad>/planet_frame.png` — oblique aerial: smooth procedural rolling hills +
  a boulder, ground visible. The clearest "it's a procedural tile" shot.
- `<scratchpad>/planet_golden.png` — low grazing sun: terrain relief with lit ridges /
  shadowed valleys (proves it's a real undulating heightfield, not a flat plane).
- `<scratchpad>/planet_final.png` — 45-deg sun: the resolved `dirtpacked09_grayrocks`
  ground **texture** is visible on the lit slope (bottom-right), proving the biome->texture chain reaches the screen.

(`<scratchpad>` = `/tmp/claude-1000/-home-vince-Documents-Projects-recreation/9bf96255-8934-4fe3-98c0-61c61c97bf20/scratchpad/`)

## What is real vs stubbed/hardcoded

**Real:** the `.biom` decode; the biome record read; the biome->LTEX->.mat->texture
resolution (real Starfield ground textures, verified across dirt/snow/sand on a
5-biome planet); the deterministic heightfield; the terrain mesh/collider/renderable
realization; the biome-map cell sampling that varies ground across the tile.

**Synthetic / stubbed (by design for the MVP):**
- **Terrain SHAPE is engine-native noise, not Bethesda's real per-planet geometry.**
  The real shape lives in the undocumented `.btd` (BTDB) overlay-composition system,
  which this MVP deliberately sidesteps (as scoped).
- **Scatter is procedural boulders, not PKIN POIs / GRAS flora.** The engine already
  has `SpawnPackIn` and the grass baker; wiring biome `GNAM` packins + grass through
  them is the obvious next step, left out to keep the slice small.
- **Sky/lighting is the default physical atmosphere.** An airless barren moon renders
  with the bright default sky, so auto-exposure washes the pale gray ground in some
  framings (why the good shots use a lower/grazing sun). Cosmetic, not a gen bug.
- The tile is a fixed bounded block (no orbit<->surface flow, no boundary wall).

## Non-regression

- **Authored Starfield (New Atlantis)** — default boot (no planet flag) still streams
  774 exterior cells and renders the city/bridge/water/trees. `<scratchpad>/regress_newatlantis.png`.
- **Skyrim (Whiterun, `RX_CAM="321.7,60,400,0,-0.38" RX_DISTANT_LOD=1`)** — still
  streams Tamriel (11186 cells) and renders the walled city + tundra + horizon LOD.
  `<scratchpad>/regress_whiterun.png`.
- The shared `cell_streaming`/`land_baker` paths were reused, not modified (`PlanetTile`
  is a standalone generator), so the regression surface is small.

## Build / tests

- Builds clean (recreation compiles sibling rx) with the standard nix command
  (three `--override-input`: zetanet, nanobuf, rx). Exit 0, binary boots + selects the RTX 3080 Ti.
- `ctest`: **76/76 pass** (75 baseline + the new `biomtest`).

## Honest gap to real Starfield planets

1. **Terrain shape**: real planets get their geometry from the `.btd` overlay heightfield
   library composited by the surface-block/pattern system (`SFBK`/`SFPT`), whose runtime
   composition rules are community-inferred, not verified. This MVP substitutes noise.
2. **Scatter/POIs**: real biomes place `GNAM` packins + `GRAS`/flora modulated by the
   `DensityMaps.ba2`; here it's procedural boulders. The engine's `SpawnPackIn`/grass
   paths are ready to take over.
3. **Biome-map projection**: I sample hemisphere 0 as a flat raster patch; the real
   256x256 grids are a polar/azimuthal projection over the globe.
4. **Planet metadata (PNDT/STDT)**: radius/gravity/parent-star are not read (not needed
   for a surface tile, but required for orbit + physics fidelity).
5. **Orbit<->surface flow, boundary wall, per-player POI seeding, tile-fits-neighbour
   geography** — all out of scope for a single-tile MVP.
6. **rx factoring**: the noise/heightfield helper is small and recreation-local; it was
   not worth extracting into rx as a generic capability yet. If a second consumer
   appears, `HeightBethesda`/the FBM value noise is the clean extraction candidate.

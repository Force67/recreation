# Starfield architecture/ship textures via the materialsbeta.cdb object graph

Starfield resolves a mesh's `.mat` material to textures only through
`engine/bethesda/material_db.cc` (`StarfieldMaterialDb`). The shipped reader did a
linear name-convention scan: it walked the CDB's `DIFF` chunks and recovered a
material's textures only when its TextureSet object was named
`<material>_TextureSet<n>` (mostly landscape/props). Architecture and ship
materials do not use that naming; they reach their textures through the CDB's
object-graph edges (Material -> layered material -> TextureSet by object id), so
they fell back to the path convention (which does not exist for architecture) and
rendered with the default 0.5 gray `base_color_factor`.

This change parses the real BSComponentDB2 object graph and resolves a material's
textures by id linkage, keyed by the `.mat` path hash.

## The BSComponentDB2 / CDB layout recovered

`materialsbeta.cdb` (105 MB) is a `BETH` reflection stream. Layout (reverse
engineered from gibbed/Gibbed.Starfield and fo76utils/nifskope `bsmatcdb.cpp` /
`matcomps.cpp`, then validated byte-for-byte against the shipped file):

- Header: `"BETH"`(u32), `headerSize`=8, `version`=4, `chunkCount`. Then a stream
  of chunks, each `[type u32][size u32][data]`.
- STRT: string table; strings live at byte offsets and are null-terminated.
- TYPE: a single u32 = the number of classes.
- CLAS (one per class, 97 total): `nameOffset(s32 -> STRT)`, `classVersion(u32)`,
  `flags(u16)` (bit 2 = "is user/chunk-carrying"), `fieldCount(u16)`, then per
  field `nameOff(u32)`, `typeOff(u32)`, `dataOffset(u16)`, `dataSize(u16)`. A field
  type is either a STRT offset (a class) or a builtin encoded as `>= 0xFFFFFF01`
  (`0xFFFFFF02`=String, `0xFFFFFF0D`=UInt32, etc.).
- Three index LIST chunks carry the graph:
  - `BSComponentDB2::DBFileIndex::ObjectInfo` - the object table. Each entry
    (21 bytes, or 33 for files >= 1.11.33.0): `persistentID{file,ext,dir}` (12B,
    the BSResourceID), `dbID(u32 @12)`, `baseObjectID(u32 @16)`, trailing
    `hasData` byte. `persistentID` = the `.mat` path CRC.
  - `BSComponentDB2::DBFileIndex::ComponentInfo` - the component table. Each entry
    `dbID(u32)`, `key(u32 = (type<<16)|index)`. Consumed positionally: the Nth
    following OBJT/DIFF chunk binds to `componentInfo[N]`.
  - `BSComponentDB2::DBFileIndex::EdgeInfo` - the parent/child hierarchy (12B:
    sourceID, targetID, index, type). Not needed here; the texture links come
    from the id components below.
- OBJT / DIFF component chunks (~1.44M): each starts with a `className(u32 ->
  STRT)`, then a schema-typed value. OBJT reads fields sequentially; DIFF reads a
  `u16` field-index loop terminated when the index goes negative/out of range. A
  String value is `[u16 len][bytes]`.

The class/field ids that matter (STRT names):

| Class | Role |
| --- | --- |
| `BSMaterial::LayerID` | id component: Material -> Layer (slot = key index) |
| `BSMaterial::MaterialID` | id component: Layer -> Material |
| `BSMaterial::TextureSetID` | id component: Material -> TextureSet |
| `BSComponentDB2::ID` | the id wrapper (one `UInt32 Value` = target dbID) |
| `BSMaterial::TextureFile` / `MRTextureFile` | one `String FileName`; key index = slot |

TextureSet slot indices (from nifskope `material.hpp`): 0 = base color, 1 = normal,
7 = emissive.

Resolution per material: `Material(has LayerID) -> layer -> MaterialID -> material
-> TextureSetID -> TextureSet -> TextureFile[slot 0/1/7]`. Base layer first, so the
base color wins.

The `.mat` lookup hashes the path into a BSResourceID the same way the engine
resource system does (`dir` = CRC32 of the lowercased directory with `/`->`\`,
`file` = CRC32 of the lowercased base name, `ext` = packed lowercased extension;
poly 0xEDB88320, no init/final xor). Verified against the shipped table:
`Materials\Architecture\SpaceStationKit\StnDividerWallTrim01.mat` ->
`file=3AE31F9A ext=0074616D dir=54C07ABF`, matching object dbID 568.

## Changes

- `engine/bethesda/material_db.h` / `.cc`: the graph parse
  (`BuildGraphIndex`), keyed by BSResourceID (`by_resource_`), plus the public
  `HashResource`. The old TextureSet-name scan is kept as `BuildStemIndex`
  (`by_stem_`, fallback), so nothing that resolved before regresses. `Lookup`
  tries the graph first, then the stem index. `Build` runs both. `graph_size()`
  reports the object-graph count.
- `engine/bethesda/converters.cc`: the load log now reports the graph count:
  `starfield material database: 77829 materials indexed (41398 via object graph)`.
- `tools/papyrus/material_dbtest.cc`: adds a `HashResource` check (against the
  verified shipped value) and a synthetic-graph round trip that wires a material
  down to its texture set and confirms it resolves by path hash. 17 checks, all
  green.

## Coverage: before vs after

Object table: 500403 objects, 1438778 components, 451529 edges, 97 classes (whole
file parses cleanly, every component chunk consumed).

Top-level materials in the database: 48663. Base color resolved through the graph:
38988 (80.1%).

`StarfieldMaterialDb::size()` grew 36431 -> 77829 materials indexed, of which
41398 resolve through the object graph (the path that reaches architecture and
ships).

On a realistic workload of 862 real `.mat` paths taken from the shipped string
table (the harder tail, most of which are not `_TextureSet`-named):

| Reader | base_color resolved | architecture resolved |
| --- | --- | --- |
| Old (TextureSet-name scan) | 3 / 862 (0.3%) | 0 / 131 (0.0%) |
| New (object graph) | 493 / 862 (57.2%) | 58 / 131 (44.3%) |

Spot-checked resolutions (texture files confirmed present in the BA2s):

- `Materials\Architecture\City\NewAtlantis\NAConcreteMossy01.mat` ->
  `textures/architecture/city/newatlantis/naconcretemossy01_color.dds` (+ normal)
- `Materials\Architecture\SpaceStationKit\StnDividerWallTrim01.mat` ->
  `.../spacestationkit/stndividerwalltrim01_color.dds` (+ normal)
- `Materials\Architecture\City\NewAtlantis\Mast\UC_OrientationMural_NewFactionsRise02.mat`
  -> the Mast mural color.

`assetdump` on a single-material architecture NIF now loads the bound textures
(`texture ... 1024x1024 mips=11 format=2 srgb=1` color + BC5 normal), confirming
the resolution reaches the render path.

## Verification screenshots

Captured this session (deterministic `RX_CAM`, `RX_DISTANT_LOD=1`, frozen noon),
under the session scratchpad:

- `na_after_ground.png` - ground close-up: the plaza/hardscape paving renders with
  real textured, normal-mapped detail (single-material architecture surfaces).
- `na_plaza.png` - auto-placed plaza spawn over the city core.
- `na_after_aerial.png` - the report's aerial pose (`RX_CAM="150,180,480,5.8,-0.35"`).

## What still is not textured

The material database now resolves architecture/ship textures. The remaining grey
in the New Atlantis city core is a separate NIF-binding limitation, not the
material db: `ConvertStarfieldNif` binds one shared material per NIF via
`SingleMaterialPath`, which returns nothing when a NIF references more than one
distinct `.mat` (ambiguous). Sampling 60 New Atlantis architecture NIFs, 47
(78%) are multi-material - the large composite buildings - so they get no
material bound at all regardless of the CDB fix, while the 13 single-material ones
(ground, hardscape, some props) are now correctly textured. Making the city core
fully textured needs per-BSGeometry material binding (each submesh gets its own
`.mat` from the NIF, resolved through this database), a NIF-format change in
`ParseStarfieldNif` / `ConvertStarfieldNif`, not a material-db change.

Also unchanged: only the base color / normal / emissive slots are used (roughness,
metalness, AO, height are resolved by the graph but the engine material has no
slots for them); dense-city perf (2-3 fps, RT-forced LOD0) is untouched.

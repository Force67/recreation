# Starfield map render fix (feature/starfield-map-render)

New Atlantis rendered as a garbled heap: one overlapping gray building mass, cyan/mint blob
geometry, flat water flooding the whole plaza, no ground terrain. All fixed in commit
`1c39307`. The suspected causes (BLAS salting, .mesh stride) were NOT the problem.

## What was wrong

1. **Unit scale.** Starfield authors REFR/world positions in metres on a 100 m exterior
   cell grid (verified empirically: per-cell ref clusters fall in 100-unit bands). The
   streamer, persistent-ref binning, and camera start all used Skyrim's 4096-unit cells
   and 0.01428 units-to-metres. Meshes were lifted x70 (net ~1.0) but positions were
   multiplied by 0.01428, so every placement was compressed 70x toward the origin: the
   whole city rendered as one overlapping heap.
2. **NIF node translations.** `ConvertStarfieldNif` baked metre-space BSGeometry node
   translations onto x70-lifted vertices, collapsing multi-part NIFs 70x toward their own
   origin (the giant blobby masses).
3. **Vertex color.** Starfield `.mesh` vertex color is a layered-material blend mask, not
   an albedo tint; multiplying it into shading painted whole facades cyan/mint.
4. **Missing terrain.** NewAtlantis has no LAND/btd records; its natural ground ships as
   per-cell instance NIFs (`meshes/terrain/<ws>/objects/<ws>.<level>.<x>.<y>.nif`)
   containing BSWeakReferenceNode lists of (STAT form id + 3x4 transform) placements.
   These were never loaded, so the cliffs/rock ground were absent.
5. **Missing prefabs.** PKIN (pack-in) refs (spaceport buildings, retaining walls) were
   skipped entirely.
6. **Water table.** Starfield WRLD carries an XCLW/WHGT per-cell water height list (the
   upper lake sits at 242 vs the default 41). Only the default was used, so the default
   plane flooded the plaza and the camera spawned underwater.

## What changed

- `engine/bethesda/game_profile.{h,cc}`: per-game `cell_size` / `units_to_meters`
  (Starfield: 100 m cells, metre positions).
- `engine/world/cell_streaming.{h,cc}`: constructor takes the GameProfile; all
  record-position math (ToWorld, cell grid mapping, water/land/light-radius/decal/fog/
  ground conversions, water plane span) uses the per-game members while converted-mesh
  scaling keeps the fixed game-unit constant. WRLD XCLW/WHGT water-table parse with
  per-cell height override; water-aware `RefsGroundHeight` percentile (ignores submerged
  props so the camera spawns on the platform deck). `SpawnInstancedTerrain` (per-cell
  BSWeakReferenceNode STAT instances via MeshForBase, `Mat3RotationToEngine` quaternion
  from the row-major matrix). `SpawnPackIn` (PKIN prefab instantiation: template-cell
  refs composed onto the PKIN ref transform, recursive; `BethQuatFromEuler`/`QuatRotate`
  helpers). Terrain-instance counter in the streaming-idle log.
- `engine/bethesda/starfield_mesh.{h,cc}`: `ParseStarfieldInstancedNif`
  (BSWeakReferenceNode instance-list decode, byte-exact verified); vertex-color streams
  no longer applied to shading; shared `kStarfieldMetresToGameUnits` constant.
- `engine/bethesda/converters.cc`: BSGeometry node translations lifted x70 to match the
  vertex lift.
- `engine/bethesda/load_order.cc`: persistent worldspace refs binned by
  `profile.cell_size` (were all misbinned near the origin for Starfield).
- `runtime/content_load.cc`: streamer construction passes the profile; camera-start and
  secondary-domain anchors use the profile cell size/scale.
- `tools/esminfo/main.cc`: new `cellrec` (dump WRLD+CELL subrecords hex) and `form`
  (dump any record's subrecords by hex id) probe modes used for the diagnosis.
- `tools/assetdump/main.cc`: `.nif` conversion falls back to the profile-registered
  converter so Starfield BSGeometry NIFs convert (was Skyrim-only dispatch).

## What now renders (verified 2026-07-12 on a fresh RelWithDebInfo build)

New Atlantis renders as a recognizable, correctly laid-out city: the MAST-district
plateau with the tower, spaceport structures, the waterfront suspension bridge and dish,
the lake filling the basin at the authored heights, and 1430 instanced cliff/rock terrain
pieces forming the island edges. Streaming idle: 49 cells, 7252 entities, 872 meshes
converted, 1430 terrain instances, 49 water planes, 0 conversion failures. Skyrim
(Whiterun pose) renders identically to the pre-change baseline and all 75 ctests pass.

Screenshots (in `build/nix/starfield-report/`, gitignored):

| Capture | Path |
| --- | --- |
| Before (garbled heap, cyan blobs, flooded plaza) | `build/nix/starfield-report/sf_before.png` |
| After, aerial (`RX_CAM="150,180,480,5.8,-0.35"`) | `build/nix/starfield-report/sf_after_aerial.png` |
| After, default camera on the platform deck | `build/nix/starfield-report/sf_after_ground.png` |
| After, upper lake at authored height 242 | `build/nix/starfield-report/sf_after_lake.png` |
| Skyrim regression (Whiterun, unchanged) | `build/nix/starfield-report/skyrim_regression.png` |

Reproduce the aerial capture:

```
RX_HIDE_DEBUG_UI=1 RX_CAM="150,180,480,5.8,-0.35" RX_SCREENSHOT=out.png:30 \
  vkrun ./build/nix/runtime/recreation --data-dir "<Starfield>/Data"
```

## Remaining gaps / next steps

Pre-existing, not introduced by this change:

- **City architecture is largely gray** (~36% texture ceiling for architecture). Needs
  the full schema-driven BSComponentDB2 object-graph parse of `materialsbeta.cdb`
  (material -> layer -> TextureSet-by-ID edges); the current linear TextureSet-name scan
  cannot reach them. Multi-hour careful implementation, do it as a dedicated task.
- **No horizon terrain beyond the streamed radius.** Level-8 instance tiles exist but
  duplicate the level-1 placements, so wiring them into distant LOD as-is would z-fight;
  left out deliberately. Next step: use them exclusively outside the streamed radius
  (swap on cell load/unload, like the Skyrim .btr/.bto path).
- **No physics colliders on instanced terrain** (SpawnInstancedTerrain places render
  meshes only).
- **Dense-city perf** (2-3 fps in the city core): RT forces LOD0; needs RT-aware
  per-instance LOD or a reduced base LOD (known, tracked in memory).

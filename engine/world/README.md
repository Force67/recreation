# World terrain edits

`terrain_edits.h/.cc` is Recreation's non-destructive Bethesda height edit
layer. Original plugins and archives remain immutable. A diff stores sparse final
game-height deltas on the canonical LAND lattice:

```
global_x = cell_x * 32 + local_col
global_y = cell_y * 32 + local_row
```

Because a border vertex has one global coordinate, composing either neighboring
33x33 cell reads the same delta. `CellStreamer` converts editor metres and engine
`x/-z` coordinates to this lattice, composes the diff over decoded `VHGT`, and
refreshes only touched loaded cells. LAND CPU meshes retain their material and
splat submeshes; only dynamic vertices, bounds, the heightfield collider, and the
ground query result change.

## `.recterrain` v1

All integers are little-endian. The file is deterministic: cells and canonical
samples are strictly sorted. Saves write `<path>.tmp` and rename it over the
destination.

| Field | Type | Meaning |
| --- | --- | --- |
| magic | 8 bytes | `RECTERR\0` |
| version | `u16` | `1` |
| header bytes | `u16` | `40` |
| world bytes | `u32` | UTF-8 world identity length |
| cell count | `u32` | number of sorted cell records |
| sample count | `u32` | total sparse non-zero deltas |
| quantization | `u32` | `256` game-height steps per unit |
| reserved | `u32` | zero |
| payload bytes | `u64` | exact bytes after the header, before checksum |
| world identity | byte string | lower-case world EDID plus winning source plugin |
| cells | variable | records described below |
| checksum | `u32` | CRC-32 of every preceding byte |

Each cell record is `i32 x, i32 y, u64 base_fingerprint, u32 sample_count`,
followed by that canonical owner cell's samples. A sample is `u8 local_x, u8
local_y, i32 quantized_delta`; local coordinates are 0..31 because a coordinate
at 32 belongs to the next canonical cell. Zero-sample neighbor records preserve
fingerprints for LANDs sharing edited borders. The fingerprint covers the bound
world, winning source plugin, LAND identity, and winning `VHGT` bytes. A non-zero
fingerprint mismatch rejects the entire load instead of applying deltas to
changed source data.

The 1/256-game-unit quantization is about 0.0056 cm in Skyrim engine space and
has an approximately 120 km vertical range. The loader checks magic/version,
exact lengths, CRC, world binding, sorted uniqueness, local coordinates,
quantization, count/file limits, overflow, truncation, border cell coverage, and
base fingerprints before replacing the active diff.

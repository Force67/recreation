#ifndef RECREATION_BETHESDA_STARFIELD_MESH_H_
#define RECREATION_BETHESDA_STARFIELD_MESH_H_

#include <string>

#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "core/types.h"

namespace rx::bethesda {

// Starfield authors NIF node transforms and ".mesh" positions in metres; the
// rigid conversion lifts both to Bethesda game-unit object space (~70/m) so
// the shared streamer's fixed unit->metre mesh scale applies unchanged.
inline constexpr f32 kStarfieldMetresToGameUnits = 70.0f;

// Starfield (BS stream 173) replaced the inline BSTriShape geometry with the
// BSGeometry block: it carries only a node transform, bounds, and a list of LOD
// references to external ".mesh" files (keyed by hash). The geometry itself
// lives in those ".mesh" files. The NIF parser resolves each BSGeometry into a
// baked object-space node transform plus the vfs path of its highest detail
// ".mesh"; the converter then loads and decodes that mesh.

// One decoded ".mesh" geometry buffer. Positions are in Bethesda object space
// (game units, ~70 per metre) after the unit conversion baked in on parse;
// normals are smooth area-weighted face normals (the packed mesh normals use an
// undecoded format); UVs come from the first texture coordinate stream.
struct StarfieldMeshData {
  base::Vector<asset::Vertex> vertices;
  base::Vector<u32> indices;
};

// Decodes a Starfield ".mesh" blob. Returns false on a short read, an
// unsupported version, or a stream the layout does not account for.
bool ParseStarfieldMesh(ByteSpan data, StarfieldMeshData* out);

// A decoded skinned ".mesh": the same geometry the rigid path produces plus a
// per-vertex bone binding (the top four influences of the weight stream). Unlike
// the rigid path, positions stay in Starfield-native metres: a skinned body is
// driven by a metres-space skeleton, so its whole skin chain (vertices, bind
// transforms, skeleton) is kept in metres for consistency, not lifted to game
// units. Bone indices are the skin-bone indices the weight stream references,
// which the converter resolves to names through the NIF's bone list.
struct StarfieldSkinnedMeshData {
  base::Vector<asset::Vertex> vertices;
  base::Vector<asset::SkinnedVertexExtra> skinning;
  base::Vector<u32> indices;
};

// Decodes a Starfield ".mesh" blob keeping its weight stream, for runtime GPU
// skinning. Reduces each vertex's influences to the top four by weight and
// renormalizes them to u8 summing to 255. Returns false on a short read, an
// unsupported version, or a mesh that carries no weights (weightsPerVertex 0).
bool ParseStarfieldSkinnedMesh(ByteSpan data, StarfieldSkinnedMeshData* out);

// One BSGeometry resolved from a Starfield NIF: the full node-chain transform
// baked from the parents down to the geometry, the normalized vfs path of its
// highest detail ".mesh" ("geometries/<dir>/<file>.mesh"), and the ".mat"
// material path from its shader property (each geometry has its own, so a
// multi-material NIF binds a distinct material per submesh).
struct StarfieldGeometryRef {
  // Row-major 3x3 rotation, translation, uniform scale, matching the NIF
  // transform convention. Applied as rotation * p * scale + translation.
  f32 rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  f32 translation[3] = {0, 0, 0};
  f32 scale = 1.0f;
  std::string mesh_path;
  std::string material_path;  // normalized "materials/...mat", empty if none
};

// Walks a Starfield NIF node graph and collects every visible BSGeometry as a
// geometry reference with its baked world transform and resolved mesh path.
// Returns false when the header does not parse or no geometry is found.
bool ParseStarfieldNif(ByteSpan data, base::Vector<StarfieldGeometryRef>* out);

// One placement of a base form inside a Starfield terrain-instance NIF
// (meshes/terrain/<worldspace>/objects/<worldspace>.<level>.<x>.<y>.nif).
// Hand-built Starfield worldspaces have no LAND records: their natural ground
// (cliffs, rock fields) ships as these per-cell BSWeakReferenceNode instance
// lists, each entry a STAT form id + BA2-style file hashes + a run of world
// space placements (rotation rows, translation in metres, uniform scale).
struct StarfieldTerrainInstance {
  f32 rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major, p' = R * p
  f32 translation[3] = {0, 0, 0};                 // worldspace metres
  f32 scale = 1.0f;
};
struct StarfieldTerrainGroup {
  u32 form_id = 0;  // raw base form id (resolve against the owning plugin)
  base::Vector<StarfieldTerrainInstance> instances;
};

// Decodes the BSWeakReferenceNode instance lists of a Starfield terrain NIF.
// Returns false when the header does not parse or no instance list is found.
bool ParseStarfieldInstancedNif(ByteSpan data, base::Vector<StarfieldTerrainGroup>* out);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_STARFIELD_MESH_H_

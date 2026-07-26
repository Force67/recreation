#ifndef RECREATION_BETHESDA_NIF_H_
#define RECREATION_BETHESDA_NIF_H_

#include <optional>
#include <string>
#include <string_view>

#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/skeleton.h"
#include "core/types.h"

namespace rx::bethesda {

// Gamebryo/NetImmerse scene file. All three games ship 20.2.0.7 with
// different BS stream versions (SSE: 100 with BSTriShape packed vertices
// plus legacy NiTriShape for tree trunks). We do not keep the scene graph,
// geometry is flattened into one engine mesh with node transforms baked in.
// Vertices stay in Bethesda object space (Z-up, game units); the cell
// streamer owns the engine space conversion.
struct NifHeader {
  u32 version = 0;
  u32 user_version = 0;
  u32 bs_version = 0;
  // Fallout 3 / New Vegas (user version 11): NiAVObject still carries the
  // classic Properties list and NiTriShape/NiTriStrips reference their shader,
  // material and alpha through it (no trailing shader/alpha refs like Skyrim).
  bool legacy_geometry = false;
  base::Vector<std::string> block_types;
  base::Vector<u16> block_type_index;
  base::Vector<u32> block_sizes;
  base::Vector<u32> block_offsets;  // absolute offsets into the file
  base::Vector<std::string> strings;  // header string table (node/bone names)
};

std::optional<NifHeader> ParseNifHeader(ByteSpan data);

// The flattened result: one mesh with a submesh per shape, plus the
// materials those submeshes reference (synthesized from the NIF shader
// property blocks) and the texture paths the materials want loaded.
struct NifConversion {
  base::UniquePointer<asset::Mesh> mesh;
  base::Vector<asset::Material> materials;
  // Fallout 4 shader properties bind their textures through a .bgsm/.bgem
  // material file instead of an inline texture set. Parallel to `materials`:
  // the normalized "materials/..." path of each, or empty when textures were
  // inline. The asset layer reads the file to fill the missing bindings.
  base::Vector<std::string> material_files;
  // Normalized vfs paths ("textures/..."), deduplicated.
  base::Vector<std::string> texture_paths;
  u32 skipped_shapes = 0;  // strip/empty shapes we cannot use yet
  u32 refraction_shapes = 0;  // refraction-flagged shapes routed to transmission
  u32 effect_shapes = 0;  // effect-shader shapes routed to the unlit blend path
  u32 skinned_shapes = 0;  // shapes baked rigidly at their bind pose
  // Set by ConvertNifSkinnedMesh: mesh->skinned with mesh->skin populated.
  bool skinned = false;
  // Classic Gamebryo (Oblivion) source: normal-map paths were derived by the
  // "<diffuse>_n.dds" convention, so the converter existence-checks them and
  // unbinds the ones that do not resolve.
  bool gamebryo = false;
};

NifConversion ConvertNifScene(ByteSpan data, asset::AssetId id, std::string_view source_path);

// Classic Gamebryo NIFs (Oblivion, 10.1.0.106 .. 20.0.0.5): NiTriShape /
// NiTriStrips geometry with inline NiTexturingProperty materials, walked
// sequentially (these files carry no block size table). ConvertNifScene
// dispatches here when IsGamebryoNifVersion matches.
bool IsGamebryoNifVersion(ByteSpan data);
NifConversion ConvertGamebryoNif(ByteSpan data, asset::AssetId id, std::string_view source_path);

// Like ConvertNifScene but keeps skinned shapes as runtime-skinned geometry:
// vertices stay in bind space, MeshLod::skinning carries per-vertex bone
// indices/weights, and mesh->skin names the bones to match against a skeleton.
// Non-skinned shapes in the file are dropped. Used for actor body parts.
NifConversion ConvertNifSkinnedMesh(ByteSpan data, asset::AssetId id, std::string_view source_path);

// Like ConvertNifScene but keeps shapes whose skeleton is external (head, hair)
// as static geometry in their bind pose, for rigid attachment to one bone.
NifConversion ConvertNifRigid(ByteSpan data, asset::AssetId id, std::string_view source_path);

// Builds a skeleton from a NIF node hierarchy (skeleton.nif or a self-contained
// creature). Bones are ordered parents-before-children; transforms are the
// node local binds in Bethesda object space. Returns false if no nodes parse.
bool ConvertNifSkeleton(ByteSpan data, asset::AssetId id, asset::Skeleton* out);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_NIF_H_

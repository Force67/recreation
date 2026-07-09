#ifndef RECREATION_BETHESDA_CONVERTERS_H_
#define RECREATION_BETHESDA_CONVERTERS_H_

#include <string>

#include "asset/asset_database.h"
#include "asset/texture.h"
#include "base/memory/unique_pointer.h"
#include "bethesda/game_profile.h"
#include "core/types.h"

namespace rx::bethesda {

// Decodes a DDS blob (DX10 or legacy FourCC header) into an engine texture,
// passing BCn block data straight through. `path` drives the sRGB-vs-linear
// decision for the ambiguous UNORM color formats. Returns null on an
// unsupported or truncated source. Exposed for the asset layer and tests.
base::UniquePointer<asset::Texture> ConvertDds(ByteSpan data, asset::AssetId id,
                                               std::string_view path);

// The subset of a Fallout 4 BGSM (lighting) / BGEM (effect) material file the
// engine uses: the base-colour and normal texture paths (normalized vfs paths;
// normal is empty for BGEM) and, for BGSM, the smoothness mapped to PBR
// roughness. roughness is negative when unknown, so a caller keeps its default.
struct BgsmMaterial {
  std::string diffuse;
  std::string normal;
  f32 roughness = -1.0f;
};

// Parses a version-2 BGSM/BGEM material file. Returns false for another version,
// a non-material blob, or a short read. Exposed for the asset layer and tests.
bool ParseBgsm(ByteSpan data, BgsmMaterial* out);

// Forward declaration: the Starfield material index, defined in material_db.h.
class StarfieldMaterialDb;

// Converts a Starfield skinned body NIF (a BSGeometry plus BSSkin::Instance /
// BoneData / SkinAttach) into a runtime-skinned asset::Mesh: vertices stay in
// bind space (metres), MeshLod::skinning carries the per-vertex bone
// indices/weights, and mesh->skin names the skin bones with their inverse-bind
// transforms so a skeleton can drive it. Textures are resolved exactly as
// ConvertStarfieldNif. Returns null when the NIF carries no skinned geometry.
base::UniquePointer<asset::Mesh> ConvertStarfieldSkinnedNif(asset::AssetDatabase& database,
                                                            const StarfieldMaterialDb& mat_db,
                                                            ByteSpan data, asset::AssetId id,
                                                            std::string_view path);

// Hooks the bethesda format converters into the asset database:
//   .nif        -> asset::Mesh
//   .dds        -> asset::Texture (BCn passthrough)
//   .bgsm/.bgem -> asset::Material (FO4/76 material files)
// SSE has no material files, its shader data lives in the NIF shader
// property blocks and is emitted as synthetic materials during NIF
// conversion.
void RegisterConverters(asset::AssetDatabase& database, const GameProfile& profile);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_CONVERTERS_H_

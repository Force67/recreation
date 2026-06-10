#ifndef RECREATION_BETHESDA_CONVERTERS_H_
#define RECREATION_BETHESDA_CONVERTERS_H_

#include "asset/asset_database.h"
#include "bethesda/game_profile.h"

namespace rec::bethesda {

// Hooks the bethesda format converters into the asset database:
//   .nif        -> asset::Mesh
//   .dds        -> asset::Texture (BCn passthrough)
//   .bgsm/.bgem -> asset::Material (FO4/76 material files)
// SSE has no material files, its shader data lives in the NIF shader
// property blocks and is emitted as synthetic materials during NIF
// conversion.
void RegisterConverters(asset::AssetDatabase& database, const GameProfile& profile);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_CONVERTERS_H_

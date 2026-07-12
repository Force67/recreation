#ifndef RECREATION_BETHESDA_GAME_PROFILE_H_
#define RECREATION_BETHESDA_GAME_PROFILE_H_

#include <string>

#include <base/containers/vector.h>

#include "core/types.h"

namespace rx::bethesda {

enum class Game : u8 { kUnknown, kSkyrimSe, kFallout4, kFallout76, kStarfield, kOblivion, kMorrowind };

enum class ArchiveFormat : u8 { kBsa, kBa2 };

// Everything that differs between the supported games lives here, the rest
// of the module stays game agnostic. Detection looks at the data directory
// (master file names, archive extensions).
struct GameProfile {
  Game game = Game::kUnknown;
  std::string name;
  ArchiveFormat archive_format = ArchiveFormat::kBsa;
  f32 plugin_version = 0;          // HEDR version field
  // Elements stay std::string: master names feed path concatenation and
  // std::ifstream in the loaders.
  base::Vector<std::string> base_masters;
  std::string exterior_worldspace;  // editor id of the main outdoor worldspace
  // World-position units. Skyrim/Fallout author REFR positions in game units
  // (~70 per metre) on a 4096-unit exterior cell grid; Starfield authors them
  // in metres on a 100 m grid (meshes still convert into game-unit space).
  f32 cell_size = 4096.0f;        // record units per exterior cell edge
  f32 units_to_meters = 0.01428f; // record position units -> engine metres
  // Localized string file language token: Skyrim ships "english", the Fallout
  // games "en" (strings/<plugin>_<token>.strings).
  std::string string_language = "english";
  bool supports_esl = true;
  bool has_loose_script_source = true;
  // On disk record/group header size. Oblivion's TES4 headers are 20 bytes
  // (no form_version/unknown tail); everything since Skyrim uses 24.
  u32 record_header_size = 24;
  // Morrowind: flat TES3 records (16 byte headers, no GRUP groups, string ids
  // instead of form ids). The plugin loader translates them into the modern
  // record model at load (see tes3.h).
  bool flat_tes3 = false;

  static const GameProfile& For(Game game);
  static Game DetectFromDataDir(const std::string& data_dir);
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_GAME_PROFILE_H_

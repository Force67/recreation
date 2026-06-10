#ifndef RECREATION_BETHESDA_GAME_PROFILE_H_
#define RECREATION_BETHESDA_GAME_PROFILE_H_

#include <string>
#include <vector>

#include "core/types.h"

namespace rec::bethesda {

enum class Game : u8 { kUnknown, kSkyrimSe, kFallout4, kFallout76 };

enum class ArchiveFormat : u8 { kBsa, kBa2 };

// Everything that differs between the supported games lives here, the rest
// of the module stays game agnostic. Detection looks at the data directory
// (master file names, archive extensions).
struct GameProfile {
  Game game = Game::kUnknown;
  std::string name;
  ArchiveFormat archive_format = ArchiveFormat::kBsa;
  f32 plugin_version = 0;          // HEDR version field
  std::vector<std::string> base_masters;
  bool supports_esl = true;
  bool has_loose_script_source = true;

  static const GameProfile& For(Game game);
  static Game DetectFromDataDir(const std::string& data_dir);
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_GAME_PROFILE_H_

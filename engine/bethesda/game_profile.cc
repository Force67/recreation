#include "bethesda/game_profile.h"

#include <filesystem>

namespace rec::bethesda {

const GameProfile& GameProfile::For(Game game) {
  static const GameProfile skyrim_se{
      .game = Game::kSkyrimSe,
      .name = "Skyrim Special Edition",
      .archive_format = ArchiveFormat::kBsa,
      .plugin_version = 1.71f,
      .base_masters = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm",
                       "Dragonborn.esm"},
      .exterior_worldspace = "Tamriel",
  };
  static const GameProfile fallout4{
      .game = Game::kFallout4,
      .name = "Fallout 4",
      .archive_format = ArchiveFormat::kBa2,
      .plugin_version = 1.0f,
      .base_masters = {"Fallout4.esm"},
      .exterior_worldspace = "Commonwealth",
  };
  static const GameProfile fallout76{
      .game = Game::kFallout76,
      .name = "Fallout 76",
      .archive_format = ArchiveFormat::kBa2,
      .plugin_version = 1.0f,
      .base_masters = {"SeventySix.esm"},
      .exterior_worldspace = "Appalachia",
      .supports_esl = false,
      .has_loose_script_source = false,
  };
  static const GameProfile unknown{};

  switch (game) {
    case Game::kSkyrimSe: return skyrim_se;
    case Game::kFallout4: return fallout4;
    case Game::kFallout76: return fallout76;
    case Game::kUnknown: return unknown;
  }
  return unknown;
}

Game GameProfile::DetectFromDataDir(const std::string& data_dir) {
  namespace fs = std::filesystem;
  if (fs::exists(fs::path(data_dir) / "SeventySix.esm")) return Game::kFallout76;
  if (fs::exists(fs::path(data_dir) / "Fallout4.esm")) return Game::kFallout4;
  if (fs::exists(fs::path(data_dir) / "Skyrim.esm")) return Game::kSkyrimSe;
  return Game::kUnknown;
}

}  // namespace rec::bethesda

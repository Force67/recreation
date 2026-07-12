#include "bethesda/game_profile.h"

#include <filesystem>

namespace rx::bethesda {

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
      .string_language = "en",
  };
  static const GameProfile fallout76{
      .game = Game::kFallout76,
      .name = "Fallout 76",
      .archive_format = ArchiveFormat::kBa2,
      .plugin_version = 1.0f,
      .base_masters = {"SeventySix.esm"},
      .exterior_worldspace = "Appalachia",
      .string_language = "en",
      .supports_esl = false,
      .has_loose_script_source = false,
  };
  // Starfield keeps the TES4 plugin layout (HEDR 0.96, 24 byte record headers)
  // but ships BA2 v2/v3 archives and the new .mesh/.mat asset formats. Strings
  // are localized with the Fallout "en" token and live inside the archives.
  static const GameProfile starfield{
      .game = Game::kStarfield,
      .name = "Starfield",
      .archive_format = ArchiveFormat::kBa2,
      .plugin_version = 0.96f,
      .base_masters = {"Starfield.esm"},
      .exterior_worldspace = "NewAtlantis",
      .string_language = "en",
  };
  // Classic Oblivion (2006 Gamebryo): BSA v103 archives, 20 byte record
  // headers, strings inline in the records (no localization files), old
  // NiTriShape/NiTriStrips NIFs. The main worldspace is also called Tamriel.
  static const GameProfile oblivion{
      .game = Game::kOblivion,
      .name = "Oblivion",
      .archive_format = ArchiveFormat::kBsa,
      .plugin_version = 1.0f,
      .base_masters = {"Oblivion.esm"},
      .exterior_worldspace = "Tamriel",
      .string_language = "",
      .supports_esl = false,
      .has_loose_script_source = false,
      .record_header_size = 20,
  };
  // Classic Morrowind (2002 NetImmerse): flat TES3 records translated into the
  // modern model at load, BSA v0x100 archives, NetImmerse 4.0.0.2 NIFs. The one
  // exterior worldspace has no WRLD record; the translator synthesizes it.
  static const GameProfile morrowind{
      .game = Game::kMorrowind,
      .name = "Morrowind",
      .archive_format = ArchiveFormat::kBsa,
      .plugin_version = 1.3f,
      .base_masters = {"Morrowind.esm"},
      .exterior_worldspace = "Vvardenfell",
      .string_language = "",
      .supports_esl = false,
      .has_loose_script_source = false,
      .record_header_size = 16,
      .flat_tes3 = true,
  };
  static const GameProfile unknown{};

  switch (game) {
    case Game::kSkyrimSe: return skyrim_se;
    case Game::kFallout4: return fallout4;
    case Game::kFallout76: return fallout76;
    case Game::kStarfield: return starfield;
    case Game::kOblivion: return oblivion;
    case Game::kMorrowind: return morrowind;
    case Game::kUnknown: return unknown;
  }
  return unknown;
}

Game GameProfile::DetectFromDataDir(const std::string& data_dir) {
  namespace fs = std::filesystem;
  if (fs::exists(fs::path(data_dir) / "SeventySix.esm")) return Game::kFallout76;
  if (fs::exists(fs::path(data_dir) / "Starfield.esm")) return Game::kStarfield;
  if (fs::exists(fs::path(data_dir) / "Fallout4.esm")) return Game::kFallout4;
  if (fs::exists(fs::path(data_dir) / "Skyrim.esm")) return Game::kSkyrimSe;
  if (fs::exists(fs::path(data_dir) / "Oblivion.esm")) return Game::kOblivion;
  if (fs::exists(fs::path(data_dir) / "Morrowind.esm")) return Game::kMorrowind;
  return Game::kUnknown;
}

}  // namespace rx::bethesda

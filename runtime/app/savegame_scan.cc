#include "runtime/app/savegame_scan.h"

#include <base/algorithm.h>
#include <base/memory/move.h>
#include <base/option.h>
#include <base/strings/to_string.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "components/bethesda/savegame.h"
#include "core/log.h"
#include "runtime/app/engine_internal.h"

namespace rx {
namespace fs = std::filesystem;

namespace {

// One more directory to look in, for a layout none of the standard ones cover:
// a save folder moved to another drive, a portable install, a test rig. Several
// may be given, separated the way PATH is on this platform.
static base::Option<const char*> SaveDirOpt{"save.dir", nullptr, "RX_SAVE_DIR"};

// The header is a few hundred bytes; 8 KB covers every version of it plus the
// slack a future field would take, and costs one read per file.
constexpr size_t kHeaderProbeBytes = 8 * 1024;
// A file smaller than this cannot hold a header, and a file larger than this is
// not a savegame we would want to touch anyway.
constexpr u64 kMaxSaveBytes = 512ull * 1024 * 1024;

// What a game calls its folder under "My Games", and the Steam app id whose
// Proton prefix holds that folder on Linux. Skyrim is two of each: Special
// Edition and the original write to different folders under different ids, and
// both are Skyrim as far as the front-end is concerned.
struct SaveLocation {
  bethesda::Game game;
  const char* folder;  // under Documents/My Games
  const char* appid;   // Steam app id, for the Proton prefix
};

constexpr SaveLocation kLocations[] = {
    {bethesda::Game::kSkyrimSe, "Skyrim Special Edition", "489830"},
    {bethesda::Game::kSkyrimSe, "Skyrim", "72850"},
    {bethesda::Game::kFallout4, "Fallout4", "377160"},
    {bethesda::Game::kStarfield, "Starfield", "1716740"},
    {bethesda::Game::kFalloutNv, "FalloutNV", "22380"},
    {bethesda::Game::kFallout3, "Fallout3", "22300"},
    {bethesda::Game::kOblivion, "Oblivion", "22330"},
};

// The extensions a save carries. Everything else in the folder (.skse cosaves,
// .bak backups, the screenshots some tools leave) is not ours.
bool IsSaveFile(const fs::path& path) {
  const std::string ext = path.extension().string();
  return ext == ".ess" || ext == ".fos" || ext == ".sfs";
}

void AddUnique(base::Vector<base::String>& out, base::String value) {
  if (value.empty())
    return;
  for (const base::String& have : out)
    if (have == value)
      return;
  out.push_back(base::move(value));
}

// Documents/My Games/<folder>/Saves, the layout every one of these games uses.
// Under Proton the same tail hangs off the prefix, which is why this takes the
// root rather than building the whole path itself.
base::String SavesUnder(const fs::path& documents, const char* folder) {
  return (documents / "My Games" / folder / "Saves").string().c_str();
}

// Windows moves Documents when OneDrive is on, and a prefix made by an older
// Proton still calls it "My Documents". Both are cheap to offer.
void AddProfileRoots(base::Vector<fs::path>& out, const fs::path& profile) {
  if (profile.empty())
    return;
  out.push_back(profile / "Documents");
  out.push_back(profile / "OneDrive" / "Documents");
  out.push_back(profile / "My Documents");
}

// A save's own kind, which only its file name records. Skyrim writes
// "Autosave1.ess" and "Quicksave.ess"; Fallout 4 the same with .fos.
base::String KindOf(const base::String& file) {
  base::String lower;
  for (char c : file)
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower.find("autosave") != base::String::npos)
    return "Auto";
  if (lower.find("quicksave") != base::String::npos)
    return "Quick";
  return {};
}

bool ReadPrefix(const fs::path& path, size_t bytes, base::Vector<u8>& out) {
  std::ifstream f(path.string().c_str(), std::ios::binary);
  if (!f)
    return false;
  out.resize(bytes);
  f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(bytes));
  const std::streamsize read = f.gcount();
  if (read <= 0)
    return false;
  out.resize(static_cast<size_t>(read));
  return true;
}

}  // namespace

base::Vector<base::String> SaveDirectoriesFor(bethesda::Game game,
                                              const base::Vector<base::String>& steamapps,
                                              const base::String& home,
                                              const base::String& user_profile) {
  base::Vector<base::String> out;
  for (const SaveLocation& loc : kLocations) {
    if (loc.game != game)
      continue;

    // Native Windows, and a wine prefix mounted at a known profile.
    base::Vector<fs::path> documents;
    if (!user_profile.empty())
      AddProfileRoots(documents, fs::path(user_profile.c_str()));

    // Proton: the game's own prefix reproduces the whole Windows layout, so the
    // path is the Windows one with the prefix in front. Every Steam library can
    // hold a different game's prefix, so all of them are searched.
    for (const base::String& steamapp : steamapps) {
      const fs::path prefix = fs::path(steamapp.c_str()) / "compatdata" / loc.appid / "pfx" /
                              "drive_c" / "users" / "steamuser";
      AddProfileRoots(documents, prefix);
    }

    // A plain wine prefix, which is how these run outside Steam on Linux.
    if (!home.empty()) {
      const char* user = std::getenv("USER");
      const fs::path wine = fs::path(home.c_str()) / ".wine" / "drive_c" / "users";
      AddProfileRoots(documents, wine / (user ? user : "steamuser"));
      AddProfileRoots(documents, wine / "steamuser");
      // macOS and a native Linux port would put it here; harmless either way.
      AddProfileRoots(documents, fs::path(home.c_str()));
    }

    for (const fs::path& root : documents)
      AddUnique(out, SavesUnder(root, loc.folder));
  }
  return out;
}

base::Vector<base::String> SaveDirectories(bethesda::Game game) {
  // SteamCommonRoots names ".../steamapps/common"; the prefixes sit beside it
  // under the same steamapps root.
  base::Vector<base::String> steamapps;
  for (const base::String& common : SteamCommonRoots())
    AddUnique(steamapps, fs::path(common.c_str()).parent_path().string().c_str());

  const char* home = std::getenv("HOME");
  const char* profile = std::getenv("USERPROFILE");
  base::Vector<base::String> candidates =
      SaveDirectoriesFor(game, steamapps, home ? home : "", profile ? profile : "");

  // The override wins by being searched as well, not instead: a player who
  // points at one folder still has the others.
  if (const char* extra = SaveDirOpt.get(); extra != nullptr && *extra) {
#if defined(_WIN32)
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    base::String one;
    for (const char* c = extra;; ++c) {
      if (*c == kSep || *c == '\0') {
        AddUnique(candidates, one);
        one.clear();
        if (*c == '\0')
          break;
      } else {
        one.push_back(*c);
      }
    }
  }

  // Canonicalise before de-duplicating: a wine prefix points "My Documents" at
  // "Documents", so two candidates that look different are one folder and every
  // save in it would otherwise be listed twice.
  base::Vector<base::String> out;
  std::error_code ec;
  for (const base::String& dir : candidates) {
    if (!fs::is_directory(dir.c_str(), ec))
      continue;
    const fs::path real = fs::canonical(dir.c_str(), ec);
    AddUnique(out, ec ? dir : base::String(real.string().c_str()));
    ec.clear();
  }
  return out;
}

base::Vector<FoundSave> ScanSaves(bethesda::Game game, int limit) {
  base::Vector<FoundSave> found;
  std::error_code ec;
  base::Vector<u8> probe;

  for (const base::String& dir : SaveDirectories(game)) {
    for (const fs::directory_entry& entry : fs::directory_iterator(dir.c_str(), ec)) {
      if (ec)
        break;
      if (!entry.is_regular_file(ec) || !IsSaveFile(entry.path()))
        continue;
      const u64 size = static_cast<u64>(entry.file_size(ec));
      if (ec || size == 0 || size > kMaxSaveBytes)
        continue;
      if (!ReadPrefix(entry.path(), kHeaderProbeBytes, probe))
        continue;

      bethesda::SaveHeader header;
      if (!bethesda::ReadSaveHeader(ByteSpan(probe.data(), probe.size()), header, false))
        continue;  // a cosave, a backup, or a format this reader does not know

      FoundSave save;
      save.game = game;
      save.path = entry.path().string().c_str();
      save.file = entry.path().filename().string().c_str();
      save.character = header.player_name;
      save.location = header.player_location;
      save.game_time = header.game_time;
      save.kind = KindOf(save.file);
      save.number = header.save_number;
      save.level = header.player_level;
      save.played_seconds = header.in_game_seconds;
      save.size = size;
      save.shot_width = header.screenshot_width;
      save.shot_height = header.screenshot_height;
      save.shot_bpp = header.screenshot_bpp;
      save.shot_offset = header.body_offset - static_cast<u64>(header.screenshot_width) *
                                                  header.screenshot_height *
                                                  header.screenshot_bpp;

      // The file's own timestamp is the only "when" a save carries that means
      // anything to a player: the header's FILETIME is not read, and the
      // in-game date is a different question.
      const auto written = fs::last_write_time(entry.path(), ec);
      if (!ec) {
        // file_clock's epoch is its own, so this has to go through the system
        // clock rather than being read as seconds since 1970 directly.
        const auto sys = std::chrono::file_clock::to_sys(written);
        save.modified = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
      }
      found.push_back(base::move(save));
    }
    ec.clear();
  }

  std::sort(found.begin(), found.end(),
            [](const FoundSave& a, const FoundSave& b) { return a.modified > b.modified; });
  if (limit > 0 && static_cast<int>(found.size()) > limit)
    found.resize(static_cast<size_t>(limit));
  return found;
}

bool ReadSaveThumbnail(const FoundSave& save,
                       int max_w,
                       int max_h,
                       base::Vector<u8>& rgba,
                       int& out_width,
                       int& out_height) {
  if (save.shot_width == 0 || save.shot_height == 0 || save.shot_bpp < 3)
    return false;

  const u64 bytes = static_cast<u64>(save.shot_width) * save.shot_height * save.shot_bpp;
  std::ifstream f(save.path.c_str(), std::ios::binary);
  if (!f)
    return false;
  f.seekg(static_cast<std::streamoff>(save.shot_offset));
  base::Vector<u8> src;
  src.resize(static_cast<size_t>(bytes));
  f.read(reinterpret_cast<char*>(src.data()), static_cast<std::streamsize>(bytes));
  if (f.gcount() != static_cast<std::streamsize>(bytes))
    return false;

  // Integer box filter. The card is a quarter of the screenshot's size, so a
  // point sample would alias the fine detail these pictures are full of, and a
  // proper resampler is more machinery than a 260 px thumbnail is worth.
  const int src_w = static_cast<int>(save.shot_width);
  const int src_h = static_cast<int>(save.shot_height);
  int step = 1;
  while ((src_w / (step + 1)) >= max_w || (src_h / (step + 1)) >= max_h)
    ++step;
  out_width = base::Max(1, src_w / step);
  out_height = base::Max(1, src_h / step);

  const u32 bpp = save.shot_bpp;
  rgba.resize(static_cast<size_t>(out_width) * out_height * 4);
  for (int y = 0; y < out_height; ++y) {
    for (int x = 0; x < out_width; ++x) {
      u32 sum[3] = {0, 0, 0};
      int taken = 0;
      for (int sy = 0; sy < step; ++sy) {
        const int py = y * step + sy;
        if (py >= src_h)
          break;
        for (int sx = 0; sx < step; ++sx) {
          const int px = x * step + sx;
          if (px >= src_w)
            break;
          const u8* texel = src.data() + (static_cast<size_t>(py) * src_w + px) * bpp;
          sum[0] += texel[0];
          sum[1] += texel[1];
          sum[2] += texel[2];
          ++taken;
        }
      }
      if (taken == 0)
        taken = 1;
      u8* out = rgba.data() + (static_cast<size_t>(y) * out_width + x) * 4;
      out[0] = static_cast<u8>(sum[0] / taken);
      out[1] = static_cast<u8>(sum[1] / taken);
      out[2] = static_cast<u8>(sum[2] / taken);
      out[3] = 0xff;  // the alpha a save stores is not coverage, and is ignored
    }
  }
  return true;
}

}  // namespace rx

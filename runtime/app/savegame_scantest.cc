// savegame_scantest: where a game's saves live on this machine.
//
// Only the path building is checked here, and deliberately: it is the half that
// is wrong silently. A typo in "Skyrim Special Edition" or in an app id does not
// crash, it finds nothing, and "no saves" looks exactly like "no saves yet".
// Scanning itself needs somebody's game folder and is covered by running the
// menu against a real install.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>

#include "runtime/app/savegame_scan.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    ++g_failures;
}

bool Holds(const base::Vector<base::String>& paths, const char* needle) {
  for (const base::String& path : paths)
    if (path.find(needle) != base::String::npos)
      return true;
  return false;
}

base::Vector<base::String> Steamapps() {
  base::Vector<base::String> out;
  out.push_back("/games/SteamLibrary/steamapps");
  return out;
}

void TestWindowsProfile() {
  std::puts("windows profile");
  const base::Vector<base::String> dirs = rx::SaveDirectoriesFor(
      rx::bethesda::Game::kSkyrimSe, {}, "", "C:/Users/vince");

  Check("special edition under Documents",
        Holds(dirs, "C:/Users/vince/Documents/My Games/Skyrim Special Edition/Saves"));
  // The original Skyrim writes to its own folder, and both are one world here.
  Check("the original beside it",
        Holds(dirs, "C:/Users/vince/Documents/My Games/Skyrim/Saves"));
  // Documents moves when OneDrive is on, which is the default on new installs.
  Check("onedrive redirection",
        Holds(dirs, "C:/Users/vince/OneDrive/Documents/My Games/Skyrim Special Edition/Saves"));
}

void TestProtonPrefix() {
  std::puts("proton prefix");
  const base::Vector<base::String> dirs =
      rx::SaveDirectoriesFor(rx::bethesda::Game::kSkyrimSe, Steamapps(), "/home/vince", "");

  Check("skyrim se prefix",
        Holds(dirs,
              "/games/SteamLibrary/steamapps/compatdata/489830/pfx/drive_c/users/steamuser/"
              "Documents/My Games/Skyrim Special Edition/Saves"));
  Check("skyrim le prefix",
        Holds(dirs, "compatdata/72850/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim/"
                    "Saves"));
  // An older Proton prefix calls it My Documents, and a wine one is not Steam's.
  Check("older prefix layout", Holds(dirs, "steamuser/My Documents/My Games"));
  Check("plain wine prefix", Holds(dirs, "/home/vince/.wine/drive_c/users"));
}

void TestPerGame() {
  std::puts("one game's folders only");
  const base::Vector<base::String> fallout =
      rx::SaveDirectoriesFor(rx::bethesda::Game::kFallout4, Steamapps(), "/home/vince", "");
  Check("fallout 4 folder", Holds(fallout, "My Games/Fallout4/Saves"));
  Check("fallout 4 app id", Holds(fallout, "compatdata/377160/"));
  Check("no skyrim in it", !Holds(fallout, "Skyrim"));

  const base::Vector<base::String> starfield =
      rx::SaveDirectoriesFor(rx::bethesda::Game::kStarfield, Steamapps(), "", "");
  Check("starfield folder", Holds(starfield, "My Games/Starfield/Saves"));
  Check("starfield app id", Holds(starfield, "compatdata/1716740/"));

  const base::Vector<base::String> unknown =
      rx::SaveDirectoriesFor(rx::bethesda::Game::kUnknown, Steamapps(), "/home/vince", "");
  Check("an unknown game has nowhere to look", unknown.empty());
}

void TestNoDuplicates() {
  std::puts("no duplicates");
  base::Vector<base::String> twice = Steamapps();
  twice.push_back("/games/SteamLibrary/steamapps");  // the same library, listed again
  const base::Vector<base::String> dirs =
      rx::SaveDirectoriesFor(rx::bethesda::Game::kSkyrimSe, twice, "/home/vince", "C:/Users/v");

  for (size_t i = 0; i < dirs.size(); ++i)
    for (size_t j = i + 1; j < dirs.size(); ++j)
      if (dirs[i] == dirs[j]) {
        Check("a path is listed twice", false);
        return;
      }
  Check("every path is listed once", true);
}

}  // namespace

// SaveDirectories() asks Steam where its libraries are; the path building this
// test covers does not, so the test stands in for that answer rather than
// linking the engine to get one.
namespace rx {
base::Vector<base::String> SteamCommonRoots() {
  return {};
}
}  // namespace rx

int main() {
  std::puts("savegame_scantest");
  TestWindowsProfile();
  TestProtonPrefix();
  TestPerGame();
  TestNoDuplicates();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}

#ifndef RECREATION_RUNTIME_APP_SAVEGAME_SCAN_H_
#define RECREATION_RUNTIME_APP_SAVEGAME_SCAN_H_

// Finding the player's savegames on this machine, so the front-end can offer
// them. Two jobs, deliberately apart:
//
//   1. WHERE they are. A game writes its saves next to its own settings, not
//      into its install, so this is a question about the operating system and
//      about Steam rather than about the game. Under Proton the whole Windows
//      layout is reproduced inside a prefix, which is why the Linux paths are
//      the Windows ones with a long prefix in front.
//   2. WHAT each one is. Only the header, never the body: ReadSaveFile
//      decompresses 13 MB to answer questions this can answer from the first
//      few hundred bytes, and a folder holds hundreds of files.
//
// Nothing here loads a save. It reports what is on disk; resuming one is
// savegame_load.cc's job.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/game_profile.h"
#include "core/types.h"

namespace rx {

// One savegame on disk, summarised from its header. Numbers stay numbers: how
// "63 h 12 m" or "2 h ago" get worded is the menu's business, not this file's.
struct FoundSave {
  bethesda::Game game = bethesda::Game::kUnknown;
  base::String path;  // absolute, what --load-save takes
  base::String file;  // file name alone
  base::String character;
  base::String location;
  base::String game_time;  // the in-game date the header carries, as written
  // "Auto", "Quick", or empty for an ordinary save. Taken from the file name,
  // which is the only place it is recorded.
  base::String kind;
  u32 number = 0;  // the save's own counter, what the game calls Save 214
  u32 level = 0;
  f32 played_seconds = 0.0f;
  u64 modified = 0;  // unix seconds, for the sort and for "2 h ago"
  u64 size = 0;

  // The picture the game took, left on disk until something wants to draw it.
  u32 shot_width = 0;
  u32 shot_height = 0;
  u32 shot_bpp = 0;
  u64 shot_offset = 0;
};

// Every directory this game's saves could be in on this machine, whether or not
// it exists. Pure path building, so it can be tested without a Steam install.
base::Vector<base::String> SaveDirectoriesFor(bethesda::Game game,
                                              const base::Vector<base::String>& steamapps,
                                              const base::String& home,
                                              const base::String& user_profile);

// The same list, resolved against this machine (Steam libraries, $HOME, the
// Windows profile) and filtered to the directories that exist.
base::Vector<base::String> SaveDirectories(bethesda::Game game);

// Read every save this game has, newest first, at most `limit`. A file whose
// header does not parse is skipped rather than reported: the folder holds
// cosaves and backups too.
base::Vector<FoundSave> ScanSaves(bethesda::Game game, int limit = 200);

// The save's own screenshot as RGBA, box-filtered down to fit `max_w`x`max_h`.
// False when the file has none or cannot be read. Kept out of ScanSaves because
// a list of two hundred saves wants the facts and six of the pictures.
bool ReadSaveThumbnail(const FoundSave& save,
                       int max_w,
                       int max_h,
                       base::Vector<u8>& rgba,
                       int& out_width,
                       int& out_height);

}  // namespace rx

#endif  // RECREATION_RUNTIME_APP_SAVEGAME_SCAN_H_

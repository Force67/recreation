#ifndef RECREATION_SWF_STAGE_H_
#define RECREATION_SWF_STAGE_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/swf/movie.h"
#include "components/swf/vm.h"

namespace rx::swf {

// A movie's display list as live interpreter objects, with the classes the
// movie binds to them running.
//
// The translation elsewhere in this module reads the display list as data. This
// builds it as objects the script can reach: every placed sprite becomes a movie
// clip addressable by its instance name, `Object.registerClass` gives it the
// class its movie wrote, and the constructor runs. That is what turns a frame of
// placeholders into the menu the player sees, because a menu's contents are
// produced by its own code rather than authored.
//
// Timeline playback is not modelled: frame 0 is built and the frame-1 scripts
// run, which is the state a menu is in when it opens.
class Stage {
 public:
  Stage(Vm& vm, const Movie& movie);

  // Runs the movie's init blocks (which define and register its classes),
  // builds the clip tree, then runs the frame scripts.
  void Run();

  AsValue root() const { return root_; }

  // A clip by instance path from the root, e.g. "MenuHolder.MainListHolder".
  // Undefined when any step of the path is missing.
  AsValue Find(base::StringRef path) const;

  // How many clips the tree ended up with, and how many got a registered class.
  u32 clip_count() const { return clip_count_; }
  u32 classed_count() const { return classed_count_; }
  // Clips whose character is an exported symbol but whose class the movie never
  // registered. A long list here means the library code did not all run.
  const base::Vector<base::String>& unclassed() const { return unclassed_; }

 private:
  u32 BuildClip(const Timeline& timeline, u32 parent, base::StringRef name,
                u16 character, u32 depth);

  Vm& vm_;
  const Movie& movie_;
  AsValue root_;
  base::Vector<base::String> unclassed_;
  u32 clip_count_ = 0;
  u32 classed_count_ = 0;
};

}  // namespace rx::swf

#endif  // RECREATION_SWF_STAGE_H_

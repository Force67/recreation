#ifndef RECREATION_SWF_STAGE_H_
#define RECREATION_SWF_STAGE_H_

#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/swf/movie.h"
#include "components/swf/vm.h"

namespace rx::swf {

// A movie's display list as live interpreter objects, with the classes the
// movie binds to them running, and its timelines playable.
//
// The translation elsewhere in this module reads the display list as data. This
// builds it as objects the script can reach: every placed sprite becomes a movie
// clip addressable by its instance name, `Object.registerClass` gives it the
// class its movie wrote, and the constructor runs.
//
// The timeline matters as much as the objects do. A menu keeps each of its
// states on a different frame of the same clip and switches with `gotoAndStop`,
// so a frame-0 snapshot shows every state at once: the highlighted list row and
// the normal one, the panel that is up and the nine that are not. Playing the
// timeline is what resolves that, which is why a goto rebuilds the clip's
// children rather than only moving a frame counter.
class Stage {
 public:
  Stage(Vm& vm, const Movie& movie);

  // Runs the movie's library code (which defines and registers its classes),
  // builds the clip tree, then runs the root's frame scripts.
  void Run();

  AsValue root() const { return root_; }

  // A clip by instance path from the root, e.g. "MenuHolder.MainListHolder".
  // Undefined when any step of the path is missing.
  AsValue Find(base::StringRef path) const;

  // Moves a clip to a frame, rebuilding the children that frame places and
  // dropping the ones it does not. `frame` is 0-based. False when the object is
  // not a clip this stage built.
  bool Goto(const AsValue& clip, u32 frame);
  // The same by frame label, as `gotoAndStop("Selected")` names it.
  bool GotoLabel(const AsValue& clip, base::StringRef label);

  // How many clips the tree ended up with, and how many got a registered class.
  u32 clip_count() const { return clip_count_; }
  u32 classed_count() const { return classed_count_; }
  // Clips whose character is an exported symbol but whose class the movie never
  // registered. A long list here means the library code did not all run.
  const base::Vector<base::String>& unclassed() const { return unclassed_; }
  // How many frame changes the scripts asked for, for tests and diagnostics.
  u32 goto_count() const { return goto_count_; }

  // Every clip whose timeline has named frames: the ones carrying states a
  // script switches between (Normal/Selected, Left/Right, focused/disabled). A
  // host driving a translated screen needs exactly these.
  base::Vector<AsValue> StatefulClips() const;
  // The frame labels on a clip's timeline, in frame order.
  base::Vector<base::String> LabelsOf(const AsValue& clip) const;

  // Creates a clip from an exported symbol and puts it inside `parent` under
  // `name`, the way attachMovie does. Undefined when the movie exports no such
  // symbol. This is how the lists that build their own rows get them.
  AsValue Attach(const AsValue& parent, base::StringRef symbol, base::StringRef name,
                 i32 depth);
  // Takes a clip out of its parent's display list.
  bool Remove(const AsValue& clip);
  // An empty clip, for the scripts that build their own containers.
  AsValue CreateEmpty(const AsValue& parent, base::StringRef name, i32 depth);
  // A copy of a clip beside it, as duplicateMovieClip makes.
  AsValue Duplicate(const AsValue& clip, base::StringRef name, i32 depth);
  // play() / stop(): a playing clip steps a frame per tick and wraps.
  void SetPlaying(const AsValue& clip, bool playing);
  // One past the highest depth in use under `clip`, which is what a script asks
  // for before attaching something.
  i32 NextDepth(const AsValue& clip) const;

  // --- events -------------------------------------------------------------
  // Calls `handler` on the clip if it carries one, e.g. "onPress". Returns
  // whether anything ran, so a host can tell a handled click from a stray one.
  bool Dispatch(const AsValue& clip, base::StringRef handler,
                const base::Vector<AsValue>& args);
  bool Dispatch(const AsValue& clip, base::StringRef handler);
  // Sends `handler` to every clip on the stage, which is what a frame or a
  // key does. Returns how many ran.
  u32 Broadcast(base::StringRef handler);
  // One frame: fires the timers that came due, then onEnterFrame everywhere.
  u32 Tick(f64 elapsed_ms);

 private:
  // What a clip needs in order to play: the timeline it came from, where it is,
  // and what it currently has placed.
  struct ClipState {
    const Timeline* timeline = nullptr;
    u32 object = 0;
    u32 frame = 0;
    // Highest depth handed out, so getNextHighestDepth keeps climbing past both
    // the authored placements and anything attached since.
    i32 high_depth = 0;
    // Set by play(), cleared by stop(). A playing clip advances one frame per
    // tick and wraps, which is what a spinner or a fade is.
    bool playing = false;
    // depth -> instance name, so a frame change can tell which children to keep
    // and which to drop.
    base::Vector<base::Pair<u16, base::String>> placed;
  };

  u32 BuildClip(const Timeline& timeline, u32 parent, base::StringRef name,
                u16 character, u32 depth);
  // Applies a frame's display list to a clip that already exists.
  void ApplyFrame(u32 state_index, u32 frame, u32 depth);
  u32 StateIndexOf(const AsValue& clip) const;
  void InstallClipApi();

  Vm& vm_;
  const Movie& movie_;
  AsValue root_;
  base::Vector<ClipState> clips_;
  base::Vector<base::String> unclassed_;
  u32 clip_count_ = 0;
  u32 classed_count_ = 0;
  u32 goto_count_ = 0;
};

}  // namespace rx::swf

#endif  // RECREATION_SWF_STAGE_H_

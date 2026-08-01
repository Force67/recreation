#ifndef RECREATION_WORLD_CINE_CAMERA_H_
#define RECREATION_WORLD_CINE_CAMERA_H_

#include "core/types.h"

namespace rx::world {

// The camera language a dialogue scene is shot in. Bethesda's scenes carry no
// camera data at all: the game frames them procedurally off who is speaking and
// who they are speaking to, and so does this. Pure geometry plus a small cutting
// policy, so it can be unit tested and reused by every conversation in the game
// (scene cutscenes, the player's own dialogue camera, an NPC-to-NPC exchange).
//
// Positions are engine space (Y up, metres) and are the subjects' HEAD points,
// which is what the framing is built around.

struct CineFraming {
  f32 eye[3] = {0, 0, 0};
  f32 target[3] = {0, 0, 0};
};

enum class ShotKind : u8 {
  kOverShoulder,  // past the listener onto the speaker, the workhorse angle
  kReverse,       // the mirrored angle, onto the listener
  kTwoShot,       // both subjects in frame, from the side
  kCloseUp,       // tight on the speaker's face
  kWide,          // pulled back off the axis, the whole exchange in frame
};

struct ShotParams {
  f32 close = 1.05f;      // eye distance for a close-up
  f32 medium = 1.9f;      // how far behind the near subject an over-shoulder sits
  f32 wide = 4.6f;        // lateral offset of a wide/two-shot
  f32 shoulder = 0.62f;   // lateral offset of an over-shoulder, so the near head
                          // frames one edge instead of blocking the lens
  f32 lift = 0.16f;       // camera sits just above the eyeline
  f32 min_subject_gap = 0.6f;  // subjects closer than this are framed as one
};

// Frames one shot. When the two subjects coincide (or `listener` is the same as
// `speaker`), `speaker_yaw` gives the axis so a lone speaker still gets a
// sensible angle; it uses the engine's facing convention (forward = {sin, 0, -cos}).
CineFraming SolveShot(ShotKind kind, const f32 speaker_head[3], const f32 listener_head[3],
                      f32 speaker_yaw, const ShotParams& params);

// Chooses shots over time: cut when the speaker changes, alternate the reverse
// angle so a back-and-forth reads as coverage, and break up a long single speech
// with a different size. Deterministic (it counts cuts, it does not sample noise)
// so a scene shot the same way twice looks the same.
class ShotDirector {
 public:
  // Shortest a shot may hold, so two quick lines do not strobe the camera.
  void set_min_shot(f32 seconds) { min_shot_ = seconds; }
  // Longest one subject may hold the frame before the camera finds a new size.
  void set_max_shot(f32 seconds) { max_shot_ = seconds; }

  // Per frame: who is speaking (0 for nobody) and who they are addressing.
  // Returns true on the frame the shot changed.
  bool Update(f32 dt, u64 speaker, u64 addressee);
  // Drops the state so the next Update cuts fresh (a new scene, a new conversation).
  void Reset();

  ShotKind kind() const { return kind_; }
  f32 shot_time() const { return shot_time_; }
  u32 cuts() const { return cuts_; }

 private:
  ShotKind PickKind() const;

  ShotKind kind_ = ShotKind::kOverShoulder;
  u64 speaker_ = 0;
  u64 addressee_ = 0;
  f32 shot_time_ = 0;
  f32 min_shot_ = 1.1f;
  f32 max_shot_ = 6.5f;
  u32 cuts_ = 0;
  bool started_ = false;
};

// Eases a camera from `from` toward `to`. A cut is applied whole; everything else
// glides, which is what keeps a scene from snapping every time a head moves.
// `rate` is the fraction of the remaining distance covered per second.
CineFraming EaseFraming(const CineFraming& from, const CineFraming& to, f32 dt, f32 rate);

}  // namespace rx::world

#endif  // RECREATION_WORLD_CINE_CAMERA_H_

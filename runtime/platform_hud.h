#ifndef RECREATION_RUNTIME_PLATFORM_HUD_H_
#define RECREATION_RUNTIME_PLATFORM_HUD_H_

#include <array>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "script/papyrus/value.h"

namespace rec {

// A transient toast pushed by a mod through Hud.Notify (kind: 0 info, 1 success,
// 2 warning, 3 error).
struct PlatformNotice {
  std::string text;
  int kind = 0;
  float seconds = 4.0f;
};

// One chat line delivered through Hud.ChatLine: sender name, body, packed rgba8
// colour (0 = HUD default).
struct PlatformChatLine {
  std::string name;
  std::string text;
  u32 color = 0;
};

// A contextual interaction prompt ("Press E to ...") set through Hud.Prompt and
// cleared by Hud.ClearPrompt, keyed by a mod-chosen id.
struct PlatformPrompt {
  std::string id;
  std::string label;
  std::string key;
};

// A map blip placed through Hud.Blip / cleared by Hud.ClearBlip, keyed by id.
struct PlatformBlip {
  std::string id;
  f32 x = 0, y = 0, z = 0;
  std::string label;
  int sprite = 0;
  u32 color = 0;
  bool short_range = false;
};

// A scoreboard row: the player handle and its rendered cells (already formatted
// by the managed Scoreboard layer for the active columns).
struct PlatformScoreRow {
  u64 player = 0;
  std::vector<std::string> cells;
};

// The scoreboard panel state (Hud.Scoreboard sets the header, Hud.ScoreboardRow
// appends a row, Hud.HideScoreboard closes it).
struct PlatformScoreboard {
  bool open = false;
  std::string title;
  std::vector<std::string> headers;
  std::vector<PlatformScoreRow> rows;
};

// The game-agnostic sink for the C# multiplayer platform's HUD and Net calls
// (Hud.* and Net.*). A mod reaches it through Native.CallGlobal; the Papyrus guest
// routes the call here on the guest thread, and the runtime drains it onto the
// on-screen HUD each frame on the main thread. One instance serves the whole
// engine, independent of which game is primary, so a server's UI behaves the same
// in every universe. All access is guarded so the guest and main threads never
// race (mirroring the Debug.Notification queue).
class PlatformHud {
 public:
  // Guest thread: route one platform call into HUD state. Unknown (type, func)
  // pairs are ignored, so the surface can grow without the guest changing.
  void Submit(const std::string& type, const std::string& func,
              const std::vector<script::papyrus::Value>& args);

  // Main thread: take the notices / chat lines queued since the previous drain.
  std::vector<PlatformNotice> DrainNotices();
  std::vector<PlatformChatLine> DrainChat();

  // Main thread: snapshots of the sticky state, for the panels that render it.
  std::vector<PlatformChatLine> ChatLog() const;
  std::vector<PlatformPrompt> Prompts() const;
  std::vector<PlatformBlip> Blips() const;
  PlatformScoreboard Scoreboard() const;
  std::optional<std::array<f32, 3>> Waypoint() const;

  // Net.Connect target a mod requested, consumed once by the runtime.
  std::optional<std::string> TakePendingConnect();

  void Clear();

 private:
  static constexpr size_t kChatLogCap = 100;

  mutable std::mutex mu_;
  std::vector<PlatformNotice> notices_;        // drained to toasts
  std::vector<PlatformChatLine> chat_pending_; // drained to the chat box
  std::deque<PlatformChatLine> chat_log_;      // capped history for the box
  std::unordered_map<std::string, PlatformPrompt> prompts_;
  std::unordered_map<std::string, PlatformBlip> blips_;
  std::optional<std::array<f32, 3>> waypoint_;
  PlatformScoreboard scoreboard_;
  std::optional<std::string> pending_connect_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_PLATFORM_HUD_H_

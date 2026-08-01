#include <base/containers/array.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/optional.h>
#include <base/strings/xstring.h>
#include <mutex>

#include "runtime/ui/platform_hud.h"

namespace rx {
namespace {

// Small typed argument readers, tolerant of a short or mistyped argument list so a
// malformed managed call degrades instead of crashing. ArgStr returns by value:
// papyrus::Value::ToString() yields a temporary, so a reference would dangle.
base::String ArgStr(const base::Vector<script::papyrus::Value>& a, size_t i) {
  return i < a.size() ? a[i].ToString() : base::String();
}
int ArgInt(const base::Vector<script::papyrus::Value>& a, size_t i) {
  return i < a.size() ? a[i].ToInt() : 0;
}
f32 ArgF(const base::Vector<script::papyrus::Value>& a, size_t i) {
  return i < a.size() ? a[i].ToFloat() : 0.0f;
}

}  // namespace

void PlatformHud::Submit(const base::String& type,
                         const base::String& func,
                         const base::Vector<script::papyrus::Value>& args) {
  std::lock_guard<std::mutex> lock(mu_);
  if (type == "Hud") {
    if (func == "Notify") {
      notices_.push_back(
          {ArgStr(args, 0), ArgInt(args, 1), args.size() > 2 ? ArgF(args, 2) : 4.0f});
    } else if (func == "ChatLine") {
      PlatformChatLine line{ArgStr(args, 0), ArgStr(args, 1), static_cast<u32>(ArgInt(args, 2))};
      chat_pending_.push_back(line);
      chat_log_.push_back(base::move(line));
      while (chat_log_.size() > kChatLogCap)
        chat_log_.pop_front();
    } else if (func == "Prompt") {
      const base::String id = ArgStr(args, 0);
      prompts_[id] = {id, ArgStr(args, 1), ArgStr(args, 2)};
    } else if (func == "ClearPrompt") {
      prompts_.erase(ArgStr(args, 0));
    } else if (func == "Blip") {
      const base::String id = ArgStr(args, 0);
      blips_[id] = {id,
                    ArgF(args, 1),
                    ArgF(args, 2),
                    ArgF(args, 3),
                    ArgStr(args, 4),
                    ArgInt(args, 5),
                    static_cast<u32>(ArgInt(args, 6)),
                    ArgInt(args, 7) != 0};
    } else if (func == "ClearBlip") {
      blips_.erase(ArgStr(args, 0));
    } else if (func == "Waypoint") {
      waypoint_ = base::Array<f32, 3>{ArgF(args, 0), ArgF(args, 1), ArgF(args, 2)};
    } else if (func == "ClearWaypoint") {
      waypoint_.reset();
    } else if (func == "Nametag") {
      // (label, x, y, z, color)
      nametags_.push_back({ArgStr(args, 0), ArgF(args, 1), ArgF(args, 2), ArgF(args, 3),
                           static_cast<u32>(ArgInt(args, 4))});
    } else if (func == "Scoreboard") {
      // Header: title, columnCount, then one label per column. Resets the rows so
      // the per-frame rebuild starts clean.
      scoreboard_.open = true;
      scoreboard_.title = ArgStr(args, 0);
      scoreboard_.headers.clear();
      const int columns = ArgInt(args, 1);
      for (int i = 0; i < columns; ++i)
        scoreboard_.headers.push_back(ArgStr(args, 2 + i));
      scoreboard_.rows.clear();
    } else if (func == "ScoreboardRow") {
      PlatformScoreRow row;
      row.player = args.empty() ? 0 : args[0].ToInt();
      for (size_t i = 1; i < args.size(); ++i)
        row.cells.push_back(args[i].ToString());
      scoreboard_.rows.push_back(base::move(row));
    } else if (func == "HideScoreboard") {
      scoreboard_.open = false;
      scoreboard_.rows.clear();
    }
    return;
  }
  if (type == "Net") {
    if (func == "Connect") {
      pending_connect_ = ArgStr(args, 0);
    } else if (func == "SpawnObject" || func == "MoveObject") {
      // (id, model, x, y, z, rx, ry, rz)
      entity_ops_.push_back(
          {func == "SpawnObject" ? PlatformEntityOp::Kind::kSpawn : PlatformEntityOp::Kind::kMove,
           ArgInt(args, 0), ArgStr(args, 1), ArgF(args, 2), ArgF(args, 3), ArgF(args, 4)});
    } else if (func == "DeleteObject") {
      entity_ops_.push_back({PlatformEntityOp::Kind::kDelete, ArgInt(args, 0), "", 0, 0, 0});
    }
  }
}

base::Vector<PlatformNotice> PlatformHud::DrainNotices() {
  std::lock_guard<std::mutex> lock(mu_);
  return base::move(notices_);  // leaves notices_ empty
}

base::Vector<PlatformChatLine> PlatformHud::DrainChat() {
  std::lock_guard<std::mutex> lock(mu_);
  return base::move(chat_pending_);
}

base::Vector<PlatformChatLine> PlatformHud::ChatLog() const {
  std::lock_guard<std::mutex> lock(mu_);
  return {chat_log_.begin(), chat_log_.end()};
}

base::Vector<PlatformPrompt> PlatformHud::Prompts() const {
  std::lock_guard<std::mutex> lock(mu_);
  base::Vector<PlatformPrompt> out;
  out.reserve(prompts_.size());
  for (const auto& [id, p] : prompts_)
    out.push_back(p);
  return out;
}

base::Vector<PlatformBlip> PlatformHud::Blips() const {
  std::lock_guard<std::mutex> lock(mu_);
  base::Vector<PlatformBlip> out;
  out.reserve(blips_.size());
  for (const auto& [id, b] : blips_)
    out.push_back(b);
  return out;
}

PlatformScoreboard PlatformHud::Scoreboard() const {
  std::lock_guard<std::mutex> lock(mu_);
  return scoreboard_;
}

base::Optional<base::Array<f32, 3>> PlatformHud::Waypoint() const {
  std::lock_guard<std::mutex> lock(mu_);
  return waypoint_;
}

base::Vector<PlatformEntityOp> PlatformHud::DrainEntityOps() {
  std::lock_guard<std::mutex> lock(mu_);
  return base::move(entity_ops_);
}

base::Vector<PlatformNametag> PlatformHud::DrainNametags() {
  std::lock_guard<std::mutex> lock(mu_);
  return base::move(nametags_);
}

void PlatformHud::SetLocalPos(f32 x, f32 y, f32 z) {
  std::lock_guard<std::mutex> lock(mu_);
  local_pos_ = {x, y, z};
}

base::Array<f32, 3> PlatformHud::LocalPos() const {
  std::lock_guard<std::mutex> lock(mu_);
  return local_pos_;
}

base::Optional<base::String> PlatformHud::TakePendingConnect() {
  std::lock_guard<std::mutex> lock(mu_);
  base::Optional<base::String> out = base::move(pending_connect_);
  pending_connect_.reset();
  return out;
}

void PlatformHud::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  notices_.clear();
  chat_pending_.clear();
  chat_log_.clear();
  prompts_.clear();
  blips_.clear();
  waypoint_.reset();
  scoreboard_ = {};
  entity_ops_.clear();
  nametags_.clear();
  pending_connect_.reset();
}

}  // namespace rx

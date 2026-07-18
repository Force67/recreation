#include "script/obscript/obscript_runtime.h"

#include <algorithm>
#include <vector>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/log.h"
#include "core/types.h"

namespace rx::script::obscript {

int Runtime::Build(const bethesda::RecordStore& records) {
  constexpr u32 kScpt = FourCc('S', 'C', 'P', 'T');
  constexpr u32 kSctx = FourCc('S', 'C', 'T', 'X');
  int total = 0, parsed = 0;
  records.EachOfType(kScpt, [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
    ++total;
    bethesda::Record rec;
    if (!records.Parse(id, &rec)) return;
    std::string source = rec.GetString(kSctx);
    if (source.empty()) return;
    Script script;
    if (!Parse(source, &script) || script.name.empty()) return;
    block_count_ += static_cast<int>(script.blocks.size());
    for (const Script::Block& b : script.blocks) block_types_[b.type]++;
    scripts_.emplace(script.name, std::move(script));
    ++parsed;
  });
  RX_INFO("obscript: {} SCPT records, {} parsed, {} event blocks", total, parsed, block_count_);
  return parsed;
}

const Script* Runtime::Find(const std::string& name) const {
  auto it = scripts_.find(name);
  return it != scripts_.end() ? &it->second : nullptr;
}

bool Runtime::RunBlock(const std::string& name, std::string_view event, Host& host) {
  const Script* script = Find(name);
  if (!script) return false;
  Instance inst(script, &host);
  return inst.Run(event);
}

namespace {

// A host that logs every side effect instead of touching game state, so a report
// run can show what a real script would do without a live world.
class TracingHost : public Host {
 public:
  int effects = 0;
  void SetStage(std::string_view q, i32 s) override {
    RX_INFO("obscript:   {}.SetStage {}", q, s);
    ++effects;
  }
  void SetGlobal(std::string_view id, f32 v) override {
    RX_INFO("obscript:   set global {} to {}", id, v);
    ++effects;
  }
  void SetRemoteVar(std::string_view o, std::string_view v, f32 val) override {
    RX_INFO("obscript:   set {}.{} to {}", o, v, val);
    ++effects;
  }
  f32 Call(std::string_view target, std::string_view fn, const base::Vector<f32>& args,
           const base::Vector<std::string>& text) override {
    std::string s(target);
    if (!s.empty()) s += ".";
    s += std::string(fn);
    for (const std::string& t : text) s += " " + t;
    for (f32 a : args) s += " " + std::to_string(a);
    RX_INFO("obscript:   call {}", s);
    ++effects;
    return 0;
  }
};

}  // namespace

void Runtime::Report() const {
  RX_INFO("obscript report: {} scripts, {} blocks", scripts_.size(), block_count_);
  std::vector<std::pair<std::string, int>> hist(block_types_.begin(), block_types_.end());
  std::sort(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.second > b.second; });
  for (const auto& [type, count] : hist) RX_INFO("obscript:   Begin {} x{}", type, count);

  // Run every script's GameMode/OnActivate/OnTrigger block once through a tracing
  // host, up to a cap, so the log shows real scripts interpreting end to end.
  TracingHost host;
  int ran = 0, with_effects = 0;
  for (const auto& [name, script] : scripts_) {
    if (ran >= 40) break;
    for (const Script::Block& block : script.blocks) {
      if (block.type != "gamemode" && block.type != "onactivate" &&
          block.type != "ontriggerenter" && block.type != "onadd" && block.type != "onequip")
        continue;
      int before = host.effects;
      Instance inst(&script, &host);
      if (inst.Run(block.type)) {
        ++ran;
        if (host.effects > before) {
          ++with_effects;
          RX_INFO("obscript: ran {} Begin {}", name, block.type);
        }
      }
      break;
    }
  }
  RX_INFO("obscript report: interpreted {} scripts ({} produced effects), {} total effects", ran,
          with_effects, host.effects);
}

}  // namespace rx::script::obscript

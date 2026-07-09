// world_nettest: round-trips quest world commands through the replication codec,
// so the host's drained command list and the client's applied list are provably
// identical. No game data; built only with networking (recreation::net).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/types.h"
#include "net/world_replication.h"
#include "world/quest_world.h"

using Handle = std::uint64_t;  // avoid the rx::u64 / arch_types::u64 ambiguity
using rx::world::WorldCommand;
using rx::world::WorldOp;

namespace {
int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}
}  // namespace

int main() {
  std::printf("world_nettest\n");

  std::vector<WorldCommand> cmds;
  WorldCommand spawn;
  spawn.op = WorldOp::kSpawn;
  spawn.quest = 0x0100ABCD;
  spawn.handle = 0xFFFF0001;
  spawn.base = 0x0001A2B3;
  spawn.pos = {12.5f, -3.0f, 100.25f};
  spawn.is_actor = true;  // a replicated battle-soldier spawn
  spawn.team = 2;         // stormcloak side, so the client renders its armour
  cmds.push_back(spawn);

  WorldCommand move;
  move.op = WorldOp::kMove;
  move.quest = 0x0100ABCD;
  move.handle = 0x00012345;
  move.pos = {1, 2, 3};
  cmds.push_back(move);

  WorldCommand disable;
  disable.op = WorldOp::kSetEnabled;
  disable.quest = 0x0100ABCD;
  disable.handle = 0xFFFF0001;
  disable.enabled = false;
  cmds.push_back(disable);

  WorldCommand cleanup;
  cleanup.op = WorldOp::kCleanupQuest;
  cleanup.quest = 0x0100ABCD;
  cmds.push_back(cleanup);

  std::vector<rx::u8> blob = rx::net::EncodeWorldCommands(cmds);
  Check("encodes to a non-empty blob", !blob.empty());

  auto decoded = rx::net::DecodeWorldCommands(rx::ByteSpan(blob.data(), blob.size()));
  Check("decodes", decoded.has_value());
  if (decoded) {
    const auto& d = *decoded;
    Check("command count preserved", d.size() == cmds.size());
    if (d.size() == cmds.size()) {
      Check("spawn op/handle/base preserved", d[0].op == WorldOp::kSpawn &&
                                                  d[0].handle == 0xFFFF0001 &&
                                                  d[0].base == 0x0001A2B3);
      Check("spawn position preserved",
            d[0].pos[0] == 12.5f && d[0].pos[1] == -3.0f && d[0].pos[2] == 100.25f);
      Check("spawn is_actor flag preserved", d[0].is_actor == true && d[1].is_actor == false);
      Check("spawn team preserved", d[0].team == 2 && d[1].team == 0);
      Check("move quest+handle preserved", d[1].op == WorldOp::kMove && d[1].handle == 0x00012345);
      Check("disable flag preserved", d[2].op == WorldOp::kSetEnabled && d[2].enabled == false);
      Check("cleanup op+quest preserved",
            d[3].op == WorldOp::kCleanupQuest && d[3].quest == 0x0100ABCD);
    }
  }

  // A truncated blob must be rejected, not read out of bounds.
  if (!blob.empty()) {
    std::vector<rx::u8> truncated(blob.begin(), blob.begin() + blob.size() / 2);
    auto bad = rx::net::DecodeWorldCommands(rx::ByteSpan(truncated.data(), truncated.size()));
    Check("rejects a truncated blob", !bad.has_value());
  }

  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}

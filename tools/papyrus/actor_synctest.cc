// actor_synctest: end-to-end check of server-authoritative NPC movement sync --
// collect moved NPCs on the host, delta + encode, decode on a client, and apply
// by form id onto the client's existing NPC entity, interpolating to the target.
// Headless (real ECS, no renderer), built only with networking.

#include <cstdint>
#include <cstdio>

#include "bethesda/form_id.h"
#include "core/types.h"
#include "ecs/world.h"
#include "gamenet/actor_sync.h"
#include "net/replication.h"
#include "world/components.h"
#include "world/quest_world.h"

using Handle = std::uint64_t;
using rx::net::ActorReplicator;
using rx::net::ActorState;
using rx::world::FormLink;
using rx::world::Npc;
using rx::world::QuestWorld;
using rx::world::Transform;

namespace {
int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Creates an NPC entity at (x,_,z) and registers it by form handle.
rx::ecs::Entity MakeNpc(rx::ecs::World& w, QuestWorld& qw, Handle form, float x, float z) {
  rx::ecs::Entity e = w.Create();
  Transform t;
  t.position[0] = x;
  t.position[2] = z;
  w.Add(e, t);
  w.Add(e, FormLink{rx::bethesda::GlobalFormId{static_cast<rx::u16>(form >> 32),
                                                static_cast<rx::u32>(form)}});
  w.Add(e, Npc{});
  qw.Register(form, e);
  return e;
}
}  // namespace

int main() {
  std::printf("actor_synctest\n");
  const Handle kForm = (Handle{1} << 32) | 0x000ABC;

  // --- host ---
  rx::ecs::World host;
  QuestWorld host_qw(host);
  rx::ecs::Entity npc = MakeNpc(host, host_qw, kForm, 0.0f, 0.0f);

  ActorReplicator rep;
  Check("first build seeds without emitting (clients have spawn pose)",
        rep.Build(rx::net::CollectActorStates(host)).empty());

  // The host moves the NPC (e.g. shoved by a player).
  host.Get<Transform>(npc)->position[0] = 5.0f;
  std::vector<ActorState> changed = rep.Build(rx::net::CollectActorStates(host));
  Check("a moved NPC is emitted", changed.size() == 1 && changed[0].form == kForm);
  Check("emitted position is the new one", changed.size() == 1 && changed[0].pos[0] == 5.0f);

  Check("a second build with no movement emits nothing",
        rep.Build(rx::net::CollectActorStates(host)).empty());

  host.Add(npc, rx::world::Hidden{});
  Check("hidden NPCs are absent from replication snapshots",
        rx::net::CollectActorStates(host).empty());
  host.Remove<rx::world::Hidden>(npc);

  // --- wire ---
  std::vector<rx::u8> blob = rx::net::EncodeActorStates(changed);
  auto decoded = rx::net::DecodeActorStates(rx::ByteSpan(blob.data(), blob.size()));
  Check("decodes", decoded.has_value() && decoded->size() == 1);

  // --- client: same NPC loaded from cell data, still at spawn ---
  rx::ecs::World client;
  QuestWorld client_qw(client);
  rx::ecs::Entity cnpc = MakeNpc(client, client_qw, kForm, 0.0f, 0.0f);

  rx::net::ApplyActorStates(client, client_qw, *decoded, /*lerp=*/0.1f);
  Check("apply does not duplicate the entity", client_qw.Find(kForm) == cnpc);
  Check("client NPC has not snapped yet (interpolating)",
        client.Get<Transform>(cnpc)->position[0] == 0.0f);

  rx::net::TickInterpolation(client, 0.2f);  // past the lerp duration
  Check("client NPC reaches the authoritative position",
        client.Get<Transform>(cnpc)->position[0] == 5.0f);

  client.Add(cnpc, rx::world::Hidden{});
  const float hidden_x = client.Get<Transform>(cnpc)->position[0];
  ActorState hidden_update = decoded->front();
  hidden_update.pos[0] = 50.0f;
  rx::net::ApplyActorStates(client, client_qw, {hidden_update}, 0.1f);
  rx::net::TickInterpolation(client, 0.2f);
  Check("hidden NPCs ignore replicated simulation updates",
        client.Get<Transform>(cnpc)->position[0] == hidden_x);

  // Unknown form is ignored (not yet streamed / different cell).
  std::vector<ActorState> stray(1);
  stray[0].form = (Handle{9} << 32) | 0x123;
  stray[0].pos[0] = 99.0f;
  rx::net::ApplyActorStates(client, client_qw, stray, 0.1f);  // must not crash
  Check("unknown form is ignored", client_qw.Find(stray[0].form).index == rx::ecs::kInvalidEntity.index);

  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}

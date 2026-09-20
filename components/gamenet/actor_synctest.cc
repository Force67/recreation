// actor_synctest: end-to-end check of server-authoritative NPC movement sync --
// collect moved NPCs on the host, delta + encode, decode on a client, and apply
// by form id onto the client's existing NPC entity, interpolating to the target.
// Headless (real ECS, no renderer), built only with networking.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "components/bethesda/form_id.h"
#include "components/gamenet/actor_sync.h"
#include "components/world/components.h"
#include "components/world/quest_world.h"
#include "core/types.h"
#include "ecs/world.h"
#include "net/replication.h"
#include "runtime/actor/gait_rate.h"  // header-only anti foot-slide gait rate (runtime/)

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
  if (!ok)
    ++g_failures;
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

  // --- death, which a client cannot work out for itself ---
  //
  // A replica simulates no combat, and dying is not a transform change, so
  // without this on the wire a killed NPC would stop moving on every client and
  // stay standing there.
  host.Add(npc, rx::world::Dead{});
  std::vector<ActorState> died = rep.Build(rx::net::CollectActorStates(host));
  Check("a death is emitted even though nothing moved",
        died.size() == 1 && died[0].form == kForm && died[0].dead);
  Check("and only once", rep.Build(rx::net::CollectActorStates(host)).empty());

  std::vector<rx::u8> death_blob = rx::net::EncodeActorStates(died);
  auto death_decoded =
      rx::net::DecodeActorStates(rx::ByteSpan(death_blob.data(), death_blob.size()));
  Check("the dead flag survives the wire",
        death_decoded && death_decoded->size() == 1 && (*death_decoded)[0].dead);
  rx::net::ApplyActorStates(client, client_qw, *death_decoded, 0.1f);
  Check("the client puts the actor down", client.Has<rx::world::Dead>(cnpc));

  // Brought back: the tag clears the same way, from the host's word only.
  host.Remove<rx::world::Dead>(npc);
  std::vector<ActorState> revived = rep.Build(rx::net::CollectActorStates(host));
  Check("a resurrection is emitted too", revived.size() == 1 && !revived[0].dead);
  base::Vector<ActorState> revived_wire;
  for (const ActorState& a : revived)
    revived_wire.push_back(a);
  rx::net::ApplyActorStates(client, client_qw, revived_wire, 0.1f);
  Check("the client stands it back up", !client.Has<rx::world::Dead>(cnpc));

  // A client that joins after the killing blow still learns about it: the
  // first-sight rule skips a form clients already have the spawn pose for, but
  // cell data never says who is already dead.
  host.Add(npc, rx::world::Dead{});
  ActorReplicator late;
  std::vector<ActorState> first = late.Build(rx::net::CollectActorStates(host));
  Check("a late joiner is told about an already-dead actor",
        first.size() == 1 && first[0].dead);
  host.Remove<rx::world::Dead>(npc);
  ActorReplicator fresh;
  Check("but a living one still costs nothing on first sight",
        fresh.Build(rx::net::CollectActorStates(host)).empty());

  client.Add(cnpc, rx::world::Hidden{});
  const float hidden_x = client.Get<Transform>(cnpc)->position[0];
  ActorState hidden_update = decoded->front();
  hidden_update.pos[0] = 50.0f;
  rx::net::ApplyActorStates(client, client_qw, {hidden_update}, 0.1f);
  rx::net::TickInterpolation(client, 0.2f);
  Check("hidden NPCs ignore replicated simulation updates",
        client.Get<Transform>(cnpc)->position[0] == hidden_x);

  // Unknown form is ignored (not yet streamed / different cell).
  base::Vector<ActorState> stray(1);
  stray[0].form = (Handle{9} << 32) | 0x123;
  stray[0].pos[0] = 99.0f;
  rx::net::ApplyActorStates(client, client_qw, stray, 0.1f);  // must not crash
  Check("unknown form is ignored",
        client_qw.Find(stray[0].form).index == rx::ecs::kInvalidEntity.index);

  // --- anti foot-slide gait playback rate (GaitPlaybackRate) ---
  // Authored gait speeds (vanilla character clips ~1.4 walk / ~4.0 run m/s).
  const float kWalk = 1.4f, kRun = 4.0f;
  auto approx = [](float a, float b) { return a > b ? a - b < 1e-4f : b - a < 1e-4f; };
  // Identity across the authored [walk, run] blend range: the blend space already
  // authors the pose for that speed, so the clock is the natural speed/walk cadence.
  Check("rate == speed/walk at the walk clip",
        approx(rx::GaitPlaybackRate(kWalk, kWalk, kRun), 1.0f));
  Check("rate == speed/walk mid-blend",
        approx(rx::GaitPlaybackRate(2.8f, kWalk, kRun), 2.8f / kWalk));
  Check("rate == run/walk at the run clip",
        approx(rx::GaitPlaybackRate(kRun, kWalk, kRun), kRun / kWalk));
  // Sprint sits above the run clip: the correction clamps at 1.4x the run cadence
  // (chipmunk cap) instead of blowing up to sprint/walk.
  Check("sprint caps at 1.4x run cadence",
        approx(rx::GaitPlaybackRate(7.0f, kWalk, kRun), 1.4f * kRun / kWalk));
  // A slow sub-walk shuffle floors at 0.7x the walk cadence (no slow-mo slide).
  Check("sub-walk floors at 0.7x walk cadence",
        approx(rx::GaitPlaybackRate(0.5f, kWalk, kRun), 0.7f));
  // Degenerate authored speeds must not divide by zero / invert.
  Check("degenerate speeds stay finite and positive",
        rx::GaitPlaybackRate(3.0f, 0.0f, 0.0f) > 0.0f);

  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}

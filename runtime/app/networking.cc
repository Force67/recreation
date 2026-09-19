#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>
#include <base/option.h>
#include <base/optional.h>
#include <base/strings/xstring.h>

#include <cstdlib>

#include "character/character.h"
#include "components/gamenet/player_sync.h"
#include "components/quest/quest_def.h"
#include "components/script/papyrus/value.h"
#include "core/input.h"
#include "core/log.h"
#include "net/replication.h"
#include "runtime/app/engine.h"
#include "runtime/app/script_trust.h"
#include "runtime/app/server_list.h"
#include "runtime/actor/player_reconcile.h"
#include "runtime/actor/player_tuning.h"
#include "scene/components.h"

#if RECREATION_HAS_NET
#include "components/gamenet/address.h"
#include "components/gamenet/asset_stream.h"
#include "components/modstream/content_provider.h"
#include "components/modstream/content_store.h"
#include "components/modstream/mod_catalog.h"
#endif

// Engine network bringup: opens the authoritative server or replica client
// session and wires the replication sinks (quest journal, quest-driven world
// commands, NPC actor streaming, dialogue/stage requests) between the net layer
// and the script/quest systems. Built as a separate Engine translation unit;
// the whole file compiles away when RECREATION_HAS_NET is off.
namespace rx {

#if RECREATION_HAS_NET
static base::Option<bool> NetQuestLog{"net.quest.log", false, "RX_NET_QUEST_LOG"};
// Streaming-bubble radius in world units. Every joining player gets an
// interest bubble of this size and the server streams each client only the
// replicated entities inside it (per-peer delta streams instead of a
// broadcast) -- the bandwidth lever that scales the player count. 0 restores
// full-visibility broadcasting.
static base::Option<int> NetBubbleRadius{"net.bubble.radius", 128, "REC_NET_BUBBLE_RADIUS"};
// Whether a client runs a server's streamed client scripts. 1 (default) asks
// once per server on the loading screen and remembers "always"/"never" choices;
// 0 never runs streamed code; 2 runs whatever the server offers without asking.
static base::Option<int> NetStreamScripts{"net.stream_scripts", 1, "REC_NET_STREAM_SCRIPTS"};
// Whether a client pulls its own locally simulated body back onto the one the
// host simulates (see player_reconcile.h). 0 leaves the two to drift, which is
// what the model did before reconciliation existed.
static base::Option<bool> NetReconcile{"net.reconcile", true, "RX_NET_RECONCILE",
                                       "correct the local body against the host's"};
// Whether a player's swing can land on another player. Off leaves everyone able
// to fight the world but not each other, which is what a co-op server wants.
static base::Option<bool> NetPvp{"net.pvp", true, "RX_NET_PVP",
                                 "players can hit each other"};
// Health one connected player swing removes, out of a pool of 100.
static base::Option<int> NetMeleeDamage{"net.melee.damage", 42, "RX_NET_MELEE_DAMAGE",
                                        "health a connected player swing removes"};

// What the script-trust policy says to do with a server's client-script offer.
enum class ScriptOfferAction {
  kRun,      // the convar or a stored decision says run, without asking
  kDecline,  // the convar or a stored decision says never run them
  kAsk,      // no stored decision: hold the loading screen on the question
};

// Resolves the policy for a server's offer, given its trust-store key.
static ScriptOfferAction ScriptOfferActionFor(const base::String& server_key) {
  const int policy = NetStreamScripts.get();
  if (policy == 0)
    return ScriptOfferAction::kDecline;
  if (policy == 2)
    return ScriptOfferAction::kRun;
  switch (ScriptTrust::DecisionFor(server_key)) {
    case ScriptTrust::Decision::kAlways:
      return ScriptOfferAction::kRun;
    case ScriptTrust::Decision::kNever:
      return ScriptOfferAction::kDecline;
    case ScriptTrust::Decision::kAsk:
      return ScriptOfferAction::kAsk;
  }
  return ScriptOfferAction::kAsk;
}

// --- server-simulated remote players -----------------------------------------
//
// A remote player is a real body: rx drops each joining player's entity with a
// bare transform, and the sinks below assemble the same character pipeline the
// local player uses (player_tuning.h) onto it. Each tick the session hands the
// simulator a peer's newest input; the simulator maps it onto that entity's
// CharacterIntent, and the world-wide character step (the local controller's on
// a listen host, net_character_step below when headless) moves the body with
// gravity and collision. Clients author nothing: the server's transform
// snapshots are the truth, and gait animation derives from the replicated
// velocity like every NPC's does.

// PlayerInput button bits (see the client-side fill in the net tick system).
constexpr u8 kInputJump = 1 << 0;
constexpr u8 kInputCrouch = 1 << 1;
constexpr u8 kInputGaitWalk = 1 << 2;
constexpr u8 kInputGaitSprint = 1 << 3;

// Assembles the character pipeline onto a joining player's entity and moves it
// from rx's hardcoded join coordinates to the game's start position. Idempotent
// per entity (the simulator also calls this defensively on first input).
static void PrepareRemotePlayer(ecs::World& world,
                                physics::PhysicsWorld& physics,
                                bethesda::RecordStore& records,
                                const Vec3& spawn,
                                ecs::Entity player) {
  if (world.Has<character::CharacterBody>(player))
    return;

  const character::CharacterMovementSettings move = player_tuning::BuildMovementSettings(records);
  const character::CharacterShape shape = player_tuning::BuildCharacterShape();
  const f32 radius = shape.standing_radius;
  const f32 half_height = base::Max(shape.standing_height * 0.5f - radius, 0.01f);

  // Feet at the game's spawn; the capsule hangs above as for the local player.
  world.Add(player, scene::Transform{.position = {spawn.x, spawn.y, spawn.z}});
  world.Add(player, move);
  world.Add(player, shape);
  world.Add(player, character::CharacterIntent{});
  world.Add(player, character::CharacterState{});
  // First person hard-locks body facing to the look yaw the client streams, so
  // the remote body faces exactly where its player looks.
  world.Add(player, character::CharacterViewMode{
                        .kind = character::CharacterViewKind::kFirstPerson});
  const physics::CharacterId cid = physics.CreateCharacter(
      {spawn.x, spawn.y + half_height + radius, spawn.z}, radius, half_height);
  if (cid != 0)
    world.Add(player, character::CharacterBody{cid, radius, half_height, false});
  world.Add(player, world::PlayerAvatar{});
}

// The entity's network id, for addressing it in avatar/vitals messages.
static u64 NetIdOf(ecs::World& world, ecs::Entity player) {
  if (const auto* id = world.Get<net::NetworkId>(player))
    return id->value;
  return 0;
}

// Tells every client which entity is this player's body and what it looks like.
static void BroadcastPlayerAvatar(rx::net::ServerSession& engine, u64 net_id, u64 form) {
  if (net_id == 0)
    return;
  engine.Broadcast(static_cast<u16>(net::GameMessage::kPlayerAvatar),
                   net::EncodePlayerAvatar({net_id, form}),
                   /*reliable=*/true, tx::network::PacketPriority::Medium);
}

// A new joiner needs the avatar table of everyone already in the world, or it
// would render the existing players as invisible transforms until they leave.
static void SendPlayerAvatarsTo(rx::net::ServerSession& engine, ecs::World& world, u32 peer) {
  world.Each<net::NetworkId, world::PlayerAvatar>(
      [&](ecs::Entity, net::NetworkId& id, world::PlayerAvatar& avatar) {
        engine.SendTo(peer, static_cast<u16>(net::GameMessage::kPlayerAvatar),
                      net::EncodePlayerAvatar({id.value, avatar.base.packed()}),
                      /*reliable=*/true, tx::network::PacketPriority::Medium);
      });
}

// Maps one peer's input onto its entity's character intent. Runs inside the
// session's per-player simulation, on the thread that owns the ECS.
static void SimulateRemotePlayer(ecs::World& world,
                                 physics::PhysicsWorld& physics,
                                 bethesda::RecordStore& records,
                                 const Vec3& spawn,
                                 ecs::Entity player,
                                 const net::PlayerInput& input,
                                 f32 dt) {
  (void)dt;
  PrepareRemotePlayer(world, physics, records, spawn, player);
  auto* intent = world.Get<character::CharacterIntent>(player);
  if (!intent)
    return;
  // The client streams its local character intent: a world-space planar move
  // request (magnitude is the analog throttle), look yaw, and edge buttons.
  intent->move = {input.move_x, 0.0f, input.move_z};
  intent->jump = (input.buttons & kInputJump) != 0;
  intent->crouch = (input.buttons & kInputCrouch) != 0;
  intent->gait = (input.buttons & kInputGaitSprint) ? character::CharacterGait::kSprint
                 : (input.buttons & kInputGaitWalk) ? character::CharacterGait::kWalk
                                                    : character::CharacterGait::kRun;
  if (auto* state = world.Get<character::CharacterState>(player))
    state->yaw = input.yaw;
}

// --- melee the host resolves --------------------------------------------------
//
// A swing is a request (see player_sync.h). A client cannot be trusted to say
// what it hit, and does not even know where anyone really is, since the host
// simulates every body; so it sends the aim it swung along and the host resolves
// that against the transforms it owns, with the same reach, arc and damage the
// single-player melee driver uses.

// Health a joining player starts with. A pool rather than the game's own actor
// value: a remote player has no actor record on the host to read one from. Mod
// code that wants the game's numbers sets them with Player.SetHealth.
constexpr u16 kJoinHealth = 100;
// The form a networked player's blows are attributed to. Every Bethesda game
// keeps its player at this id, and a remote player has no actor record of its
// own here, so this is the closest thing to the truth a script can be told.
constexpr u64 kNetPlayerFormHandle = 0x14;

// Everyone a swing could land on, gathered from the bodies the host owns:
// other players (when PvP is on) and the streamed NPCs, minus the attacker and
// anyone already down. `id` carries the peer for a player and the packed form
// for an NPC; `players` is how many entries at the front are players.
static base::Vector<world::MeleeCandidate> MeleeCandidatesAround(
    net::GameServerSession& session,
    ecs::World& world,
    ecs::Entity attacker,
    mem_size* players) {
  base::Vector<world::MeleeCandidate> out;
  if (NetPvp.get()) {
    session.engine().ForEachPeer([&](u32 peer) {
      const ecs::Entity body = session.engine().PlayerOf(peer);
      if (body == ecs::kInvalidEntity || body == attacker)
        return;
      bool dead = false;
      if (!session.PlayerVitalsOf(peer, nullptr, nullptr, &dead) || dead)
        return;  // no pool yet, or already down
      const auto* transform = world.Get<scene::Transform>(body);
      if (!transform)
        return;
      world::MeleeCandidate candidate{.id = peer};
      candidate.position[0] = transform->position[0];
      candidate.position[1] = transform->position[1];
      candidate.position[2] = transform->position[2];
      out.push_back(candidate);
    });
  }
  *players = out.size();

  world.Each<world::Npc, world::FormLink, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::FormLink& link, world::Transform& transform) {
        if (world.Has<world::Dead>(e))
          return;
        world::MeleeCandidate candidate{.id = link.form.packed()};
        candidate.position[0] = transform.position[0];
        candidate.position[1] = transform.position[1];
        candidate.position[2] = transform.position[2];
        out.push_back(candidate);
      });
  return out;
}

// Client side: the other end of that model. The host's copy of this player is a
// replicated entity like any other, so the correction is a comparison between
// its transform and the body this client simulates for itself. The policy (and
// its test) lives in player_reconcile.h; this finds the two positions and
// applies the answer. `host_copy` is the replica of our own body, `snapping`
// the caller's memory of whether it was already being snapped last frame.
static void ReconcileLocalPlayer(ecs::World& world,
                                 f32 dt,
                                 ecs::Entity host_copy,
                                 ActorSystem& actors,
                                 bool* snapping) {
  if (host_copy == ecs::kInvalidEntity)
    return;  // no snapshot has carried our own body yet
  const auto* replicated = world.Get<scene::Transform>(host_copy);
  Vec3 local{};
  if (!replicated || !actors.PlayerWorldPos(&local))
    return;
  const Vec3 server{replicated->position[0], replicated->position[1], replicated->position[2]};
  // The host has not assembled this body yet: its entity still sits where rx
  // drops a joining player, which is not a position to be corrected onto.
  if (server.x == 0.0f && server.y == 0.0f && server.z == 0.0f)
    return;

  const player_reconcile::Correction correction = player_reconcile::Reconcile(local, server, dt);
  if (correction.action == player_reconcile::Action::kAccept) {
    *snapping = false;
    return;
  }
  actors.TeleportPlayer(correction.position.x, correction.position.y, correction.position.z);
  const bool snap = correction.action == player_reconcile::Action::kSnap;
  // Logged on the way into snapping only: a body the host holds somewhere this
  // client can never reach would otherwise print every frame forever.
  if (snap && !*snapping) {
    RX_INFO("net: snapped the local body onto the host's ({:.1f}, {:.1f}, {:.1f})", server.x,
            server.y, server.z);
  }
  *snapping = snap;
}

bool StartNetworking(Engine& engine) {
  Engine* const self = &engine;
  net::GameSessionConfig net_config;
  net_config.port = self->config_.port;
  net_config.player_name = base::NameString(self->config_.player_name.c_str());
  net_config.max_clients = self->config_.max_clients;
  // Players have no placeholder mesh: a joining player's entity is an invisible
  // transform until the spawn sink assembles its real body (below), which the
  // actor system renders from the game's own assets.
  net_config.player_mesh = 0;
  net_config.bubble_radius = static_cast<f32>(NetBubbleRadius.get());

  // Asset streaming endpoints: the host catalogs its mods directory to offer, a
  // connecting client opens the content cache it streams into. A configured but
  // unreadable mods directory is a hard error, not a silent skip.
  if (self->config_.host_server) {
    if (!self->config_.mods_dir.empty()) {
      base::Optional<modstream::ModCatalog> catalog =
          modstream::ModCatalog::Build(self->config_.mods_dir.c_str());
      if (!catalog) {
        RX_ERROR("net: could not catalog mods directory '{}'", self->config_.mods_dir.c_str());
        return false;
      }
      RX_INFO("net: offering {} mod files ({} bytes) from {}", catalog->manifest().TotalFiles(),
              catalog->manifest().TotalBytes(), self->config_.mods_dir.c_str());
      self->mod_catalog_ = base::MakeUnique<modstream::ModCatalog>(base::move(*catalog));
      net_config.mod_catalog = (self->mod_catalog_ ? &*self->mod_catalog_ : nullptr);
      // Mount the host's own mods so a listen server (and headless physics/nav)
      // sees exactly what it streams to clients. Mounted after the base game, so
      // mods win, just like loose files.
      modstream::MountCatalog(*self->vfs_, *self->mod_catalog_);
    }
  } else if (!self->config_.connect_address.empty()) {
    const base::String cache_dir = self->config_.asset_cache_dir.empty()
                                       ? base::String("recreation_asset_cache")
                                       : self->config_.asset_cache_dir;
    self->content_store_ = base::MakeUnique<modstream::ContentStore>(cache_dir.c_str());
    net_config.content_store = (self->content_store_ ? &*self->content_store_ : nullptr);
  }

  if (self->config_.host_server) {
    auto server = base::MakeUnique<net::GameServerSession>(base::move(net_config));
    self->server_session_ = &*server;
    self->ctx_.server_session = self->server_session_;
    self->server_session_->SetWorldCommandSource(
        [self]() { return self->quest_world_->SnapshotDoorStates(); });
    // The shared world: the host's clock is the session's clock, and its weather
    // seed is the session's sky. Sampled every tick; the session decides what is
    // worth a message (see BroadcastWorldState).
    self->server_session_->SetWorldStateSource([self]() {
      net::WorldState state;
      if (self->clock_) {
        state.game_days = self->clock_->game_days();
        state.timescale = self->clock_->timescale();
      }
      state.weather_seed = self->director_.seed();
      state.weather = self->director_.ActiveForm(state.game_days);
      return state;
    });
    // Remote players are real bodies: assemble the character pipeline onto each
    // joining player's entity, announce its avatar to everyone, and catch a new
    // joiner up on the players already in the world.
    self->server_session_->engine().SetPlayerSpawnSink(
        [self](ecs::World& world, ecs::Entity player, u32 peer) {
          if (!self->ctx_.physics || !self->ctx_.records)
            return;
          PrepareRemotePlayer(world, *self->ctx_.physics, *self->ctx_.records, self->net_spawn_,
                              player);
          BroadcastPlayerAvatar(self->server_session_->engine(), NetIdOf(world, player), 0);
          SendPlayerAvatarsTo(self->server_session_->engine(), world, peer);
          // A pool to fight with. Mod code that wants the game's own numbers
          // overwrites it with Player.SetHealth.
          self->server_session_->SetPlayerHealth(peer, kJoinHealth, kJoinHealth, /*dead=*/false);
          RX_INFO("net: assembled the player body for peer {} at ({:.1f}, {:.1f}, {:.1f})", peer,
                  self->net_spawn_.x, self->net_spawn_.y, self->net_spawn_.z);
        });
    // The per-peer input sink: map the newest PlayerInput onto the entity's
    // CharacterIntent; the world-wide character step moves the body.
    self->server_session_->engine().SetPlayerSimulator(
        [self](ecs::World& world, ecs::Entity player, const net::PlayerInput& input, f32 dt) {
          if (!self->ctx_.physics || !self->ctx_.records)
            return;
          SimulateRemotePlayer(world, *self->ctx_.physics, *self->ctx_.records, self->net_spawn_,
                               player, input, dt);
        });
    // Headless hosts have no local player controller, so nobody else runs the
    // world-wide character step: run it here, ahead of the net tick, so the
    // bodies move with the intents written last tick before the session
    // captures this tick's snapshots. A windowed listen host skips this -- its
    // player controller's step already moves every character in the world.
    if (self->config_.headless) {
      self->scheduler_->AddSystem(ecs::Stage::kSim, "net_character_step",
                                  [self](ecs::World& world, f32 dt) {
                                    if (!self->server_session_ || self->ctx_.walk_mode)
                                      return;
                                    character::StepCharacters(world, *self->ctx_.physics, dt);
                                  });
    }
    // A client's swing: the host resolves it against the bodies it owns and
    // writes the damage into the vitals that already replicate.
    self->server_session_->SetPlayerAttackSink([self](u32 peer, f32 yaw) {
      if (!self->ctx_.world || !self->server_session_)
        return;
      ecs::World& world = *self->ctx_.world;
      net::GameServerSession& session = *self->server_session_;
      const ecs::Entity attacker = session.engine().PlayerOf(peer);
      if (attacker == ecs::kInvalidEntity)
        return;
      bool attacker_dead = false;
      if (session.PlayerVitalsOf(peer, nullptr, nullptr, &attacker_dead) && attacker_dead)
        return;  // the dead do not swing
      const auto* transform = world.Get<scene::Transform>(attacker);
      if (!transform)
        return;
      const f32 origin[3] = {transform->position[0], transform->position[1],
                             transform->position[2]};
      const f32 fwd[3] = {std::sin(yaw), 0.0f, -std::cos(yaw)};
      mem_size players = 0;
      const base::Vector<world::MeleeCandidate> around =
          MeleeCandidatesAround(session, world, attacker, &players);
      constexpr f32 kReach = 3.0f;    // as the single-player swing
      constexpr f32 kArcCos = 0.35f;  // ~70 degrees to each side
      const int picked = world::PickMeleeTarget(origin, fwd, around.data(),
                                                static_cast<int>(around.size()), kReach, kArcCos);
      if (picked < 0)
        return;

      const u16 damage = static_cast<u16>(base::Clamp(NetMeleeDamage.get(), 0, 0xffff));
      if (static_cast<mem_size>(picked) < players) {
        const u32 target = static_cast<u32>(around[picked].id);
        u16 health = kJoinHealth, max_health = kJoinHealth;
        session.PlayerVitalsOf(target, &health, &max_health, nullptr);
        const u16 left = health > damage ? static_cast<u16>(health - damage) : 0;
        session.SetPlayerHealth(target, left, max_health, /*dead=*/left == 0);
        RX_INFO("net: peer {} struck peer {} for {} ({} health left)", peer, target, damage, left);
        return;
      }
      // An NPC takes it through the guest thread, so OnHit, OnDeath and every
      // quest watching them run exactly as they do in single player. The
      // aggressor travels as the player form: a networked player has no actor
      // record of its own on the host to name instead.
      if (!self->scripts_ || !self->script_bindings_)
        return;
      auto* binds = &*self->script_bindings_;
      const u64 target = around[picked].id;
      const f32 hit_damage = static_cast<f32>(damage);
      self->scripts_->guest().Submit([binds, target, hit_damage](script::papyrus::VirtualMachine&) {
        binds->ApplyMeleeHit(script::papyrus::ObjectRef{kNetPlayerFormHandle},
                             script::papyrus::ObjectRef{target}, hit_damage);
      });
    });
    // Replicate the authoritative quest journal. The source is only called when
    // clients are connected, so the guest round-trip costs nothing while idle.
    // Quest state lives on the guest thread, so we marshal the read onto it.
    if (self->scripts_ && self->script_bindings_) {
      self->server_session_->SetQuestSource([self]() -> base::Vector<net::DomainQuestStatus> {
        // Replicate every loaded game's journal, each tagged with its domain so
        // the client routes it to the matching game.
        base::Vector<net::DomainQuestStatus> all;
        auto collect = [&](u8 domain, rx::script::ScriptSystem* scripts,
                           rx::script::skyrim::RecordBackedSkyrimBindings* binds) {
          if (!scripts || !binds)
            return;
          auto statuses = scripts->guest()
                              .SubmitFor([binds](script::papyrus::VirtualMachine&) {
                                return binds->quest_system().AllStatuses();
                              })
                              .get();
          for (quest::QuestStatus& s : statuses)
            all.push_back({domain, base::move(s)});
        };
        collect(0, (self->scripts_ ? &*self->scripts_ : nullptr),
                (self->script_bindings_ ? &*self->script_bindings_ : nullptr));
        for (size_t i = 0; i < self->extra_domains_.size(); ++i) {
          collect(static_cast<u8>(i + 1), self->extra_domains_[i]->scripts(),
                  self->extra_domains_[i]->bindings());
        }
        return all;
      });
      // The Civil War campaign board is C# state on the host only, so replicate
      // it: the session ships it to clients whenever it changes (or one joins).
      self->server_session_->SetWarMapSource([self]() -> net::WarMapState {
        net::WarMapState board;
        base::Vector<script::skyrim::RecordBackedSkyrimBindings::WarHold> holds;
        f32 fraction = 0.0f;
        self->script_bindings_->SnapshotWarMap(holds, fraction);
        board.imperial_fraction = fraction;
        board.holds.reserve(holds.size());
        for (const auto& h : holds)
          board.holds.push_back({h.name.c_str(), h.owner});
        return board;
      });
      // A client activating a reference runs OnActivate authoritatively here; the
      // resulting quest/world changes replicate back through the usual channels.
      self->server_session_->SetActivateSink([self](u32 peer, u64 handle) {
        const ecs::Entity player = self->server_session_->engine().PlayerOf(peer);
        self->interaction_->RaiseRemoteActivate(peer, player, handle);
      });
      // A client picking a dialogue topic runs that INFO's fragment here, so the
      // quest advances on the server and replicates to everyone.
      self->server_session_->SetDialogueSink(
          [self](u64 info) { self->interaction_->RunInfoFragment(info); });
      // A client's quest debugger acts through the server: apply the requested
      // stage/objective/running change on the guest, which replicates back as a
      // normal quest update.
      self->server_session_->SetStageRequestSink([self](const net::StageRequest& r) {
        if (!self->scripts_)
          return;
        auto* binds = (self->script_bindings_ ? &*self->script_bindings_ : nullptr);
        self->scripts_->guest().Submit([binds, r](script::papyrus::VirtualMachine&) {
          const script::papyrus::ObjectRef ref{r.quest};
          switch (r.op) {
            case net::StageOp::kSetStage:
              binds->SetStage(ref, r.a);
              break;
            case net::StageOp::kSetRunning:
              if (r.b)
                binds->StartQuest(ref);
              else
                binds->StopQuest(ref);
              break;
            case net::StageOp::kSetObjectiveDisplayed:
              binds->SetObjectiveDisplayed(ref, r.a, r.b != 0);
              break;
            case net::StageOp::kSetObjectiveCompleted:
              binds->SetObjectiveCompleted(ref, r.a, r.b != 0);
              break;
          }
        });
      });
    }
    // Stream authoritative NPC transforms; the session deltas them so only the
    // NPCs that actually moved this tick go out.
    self->server_session_->SetActorSource(
        [self]() { return net::CollectActorStates(*self->world_); });
    // When a client finishes streaming the mods, raise a managed event so
    // server-side C# scripts can react (gate spawn, greet the player).
    self->server_session_->SetClientReadySink([self](u32 peer) {
      RX_INFO("net: peer {} finished streaming the server's mods", peer);
      if (self->managed_)
        self->managed_->QueueEvent(
            {rx::script::host::ManagedEventId::kClientAssetsReady, peer, 0, 0, 0.0f});
    });
    // The fundamental multiplayer hooks: a player joined, a player left. Raised
    // for every peer so server-side scripts work even without streamed mods.
    self->server_session_->SetClientJoinedSink([self](u32 peer) {
      if (self->managed_)
        self->managed_->QueueEvent(
            {rx::script::host::ManagedEventId::kClientJoined, peer, 0, 0, 0.0f});
    });
    self->server_session_->SetClientLeftSink([self](u32 peer) {
      if (self->managed_)
        self->managed_->QueueEvent(
            {rx::script::host::ManagedEventId::kClientLeft, peer, 0, 0, 0.0f});
    });
    if (!server->Start()) {
      self->server_session_ = nullptr;
      self->ctx_.server_session = nullptr;
      return false;
    }
    self->session_ = base::move(server);
    // The listen host's own body: without an entity of its own, clients would
    // never see the host at all. The local player already exists (the walk-mode
    // controller assembled it), so this just puts it on the wire and announces
    // its avatar like any other player's. A dedicated server has no local
    // player, so there is nothing to publish.
    if (!self->config_.headless && self->ctx_.world && self->actors_ &&
        self->actors_->HasPlayer()) {
      ecs::World& world = *self->ctx_.world;
      const ecs::Entity host = self->actors_->PlayerEntity();
      if (!world.Has<net::NetworkId>(host)) {
        world.Add(host, net::AllocateNetworkId());
        world.Add(host, world::PlayerAvatar{});
        BroadcastPlayerAvatar(self->server_session_->engine(), NetIdOf(world, host), 0);
      }
    }
    // On the list only once the socket is actually up: an entry pointing at a
    // port nothing listens on is worse than no entry.
    StartServerAnnounce(*self);
  } else if (!self->config_.connect_address.empty()) {
    // An address from the browser (or a --connect the player typed) carries its
    // port, and the session takes host and port separately. A refusal here is
    // better than a guess: every guess ends as a connection timeout with
    // nothing to blame.
    base::String host;
    if (!net::SplitHostPort(self->config_.connect_address, &host, &net_config.port)) {
      RX_ERROR("net: '{}' is not an address to dial", self->config_.connect_address.c_str());
      return false;
    }
    net_config.address = host;
    auto client = base::MakeUnique<net::GameClientSession>(base::move(net_config));
    self->client_session_ = &*client;
    self->ctx_.client_session = self->client_session_;
    // Mirror the server's journal onto our quest system. ApplyStatus mutates
    // quest state, so it has to run on the guest thread like every other write.
    if (self->scripts_ && self->script_bindings_) {
      self->client_session_->SetQuestSink([self](u8 domain, const quest::QuestStatus& status) {
        // Route the replicated quest to the game it belongs to: 0 is the primary
        // game, 1..N the secondary domains loaded in the same order as the host.
        rx::script::ScriptSystem* scripts = nullptr;
        rx::script::skyrim::RecordBackedSkyrimBindings* binds = nullptr;
        if (domain == 0) {
          scripts = (self->scripts_ ? &*self->scripts_ : nullptr);
          binds = (self->script_bindings_ ? &*self->script_bindings_ : nullptr);
        } else if (static_cast<size_t>(domain - 1) < self->extra_domains_.size()) {
          scripts = self->extra_domains_[domain - 1]->scripts();
          binds = self->extra_domains_[domain - 1]->bindings();
        }
        if (!scripts || !binds)
          return;
        scripts->guest().Submit([binds, status](script::papyrus::VirtualMachine&) {
          // Apply via the binding (not quest_system directly) so a replicated stage
          // advance also fires the managed QuestStageChanged event, driving the C#
          // questing gameplay (XP, journal) on the client as on the host.
          binds->ApplyReplicatedStatus(status);
        });
        if (NetQuestLog)
          RX_INFO("net: applied domain {} quest 0x{:x} stage {} complete {}", domain, status.handle,
                  status.stage, status.complete ? 1 : 0);
      });
      // Mirror the host's quest-driven world effects (spawns/moves/disables/
      // cleanup). Runs in the net sim stage on the main thread, which owns the
      // ECS, so applying straight to QuestWorld is safe.
      self->client_session_->SetWorldCommandSink(
          [self](const base::Vector<world::WorldCommand>& cmds) {
            self->quest_world_->Apply(cmds);
          });
      // Mirror authoritative NPC movement onto our existing (cell-loaded) NPC
      // entities, interpolated between updates.
      self->client_session_->SetActorSink([self](const base::Vector<net::ActorState>& actors) {
        net::ApplyActorStates(*self->world_, *self->quest_world_, actors, 0.1f);
      });
      // Show the host's active objective waypoint on our own compass: store its
      // world position; UpdateObjectiveMarkers turns it into a local bearing.
      self->client_session_->SetObjectiveMarkerSink([self](const net::ObjectiveMarkerState& m) {
        self->quest_->SetRemoteMarker(m.active, Vec3{m.x, m.y, m.z});
      });
      // Mirror the host's Civil War board onto our war-map panel (the campaign C#
      // runs only on the host). SetWarHold/SetWarProgress lock the binding's
      // war-map mutex, so applying from the net sim thread is safe.
      self->client_session_->SetWarMapSink([self](const net::WarMapState& board) {
        if (!self->script_bindings_)
          return;
        for (size_t i = 0; i < board.holds.size(); ++i)
          self->script_bindings_->SetWarHold(static_cast<i32>(i), board.holds[i].name,
                                             board.holds[i].owner);
        self->script_bindings_->SetWarProgress(board.imperial_fraction);
      });
    }
    // The host's world: adopt its clock and its weather seed, so a client stands
    // in the hour everyone else does under the same sky. The clock is snapped
    // rather than eased -- both machines run at the same timescale, so after the
    // first correction the beats carry a sub-second difference nobody can see,
    // and a correction big enough to notice is one the host meant (a script set
    // the time, or we just joined).
    self->client_session_->SetWorldStateSink([self](const net::WorldState& state) {
      if (self->clock_) {
        self->clock_->set_timescale(state.timescale);
        self->clock_->set_game_days(state.game_days);
      }
      // Adopted when the host's seed CHANGES, not whenever it differs from
      // ours: aligning below can leave us on a seed of our own, and re-adopting
      // the host's on every heartbeat would undo that alignment and re-do it
      // five seconds later, forever.
      if (state.weather_seed != self->host_weather_seed_) {
        self->host_weather_seed_ = state.weather_seed;
        self->director_.AdoptSeed(state.weather_seed);
      }
      // Same seed and climate means the same weather already, and this no-ops.
      // The named form only matters where the host's climate holds a def ours
      // does not (it resumed a savegame, or a script forced a weather), which
      // the same seed would otherwise resolve to a different slot.
      self->director_.AlignWeather(state.weather, state.game_days);
    });
    // Player presence: which replicated entity is a player's body (and what it
    // looks like), and that body's replicated vitals. The entity and its
    // message arrive in either order (snapshot vs reliable channel), so an
    // unmatched offer waits in pending_avatars_/pending_vitals_ until the
    // entity spawns (drained in the net tick system above).
    self->client_session_->SetPlayerAvatarSink([self](u64 net_id, u64 form) {
      if (!self->ctx_.world)
        return;
      // Your own body is the local one: the server's copy of you stays an
      // invisible, non-solid transform so it never double-renders or shoves.
      if (self->client_session_ && net_id == self->client_session_->player_net_id())
        return;
      ecs::World& w = *self->ctx_.world;
      const ecs::Entity e = self->client_session_->replicated_entity(net_id);
      if (e == ecs::kInvalidEntity) {
        self->pending_avatars_[net_id] = form;
        return;
      }
      w.Add(e, world::PlayerAvatar{
                   bethesda::GlobalFormId{static_cast<u16>(form >> 32), static_cast<u32>(form)}});
      w.Remove<scene::Renderable>(e);
    });
    self->client_session_->SetPlayerVitalsSink([self](u64 net_id, u16 health, u16 max, bool dead) {
      if (!self->ctx_.world)
        return;
      const world::PlayerVitals vitals{health, max, dead};
      const ecs::Entity e = self->client_session_->replicated_entity(net_id);
      if (e == ecs::kInvalidEntity) {
        self->pending_vitals_[net_id] = vitals;
        return;
      }
      self->ctx_.world->Add(e, vitals);
      if (dead)
        self->ctx_.world->Add(e, world::Dead{});
      else
        self->ctx_.world->Remove<world::Dead>(e);
      // Tell client-side mods: the HUD and death handling read it from here.
      if (self->managed_)
        self->managed_->QueueEvent(
            {rx::script::host::ManagedEventId::kPlayerVitals, net_id,
             (static_cast<u64>(max) << 32) | (static_cast<u64>(health) << 16) | (dead ? 1u : 0u),
             0, 0.0f});
    });
    if (!client->Start()) {
      self->client_session_ = nullptr;
      self->ctx_.client_session = nullptr;
      return false;
    }
    self->session_ = base::move(client);
    // Mount the streamed mods into the asset Vfs once the whole manifest has
    // landed in the cache, so the host's custom content resolves like loose
    // files. The client-script offer is handled separately: it can land before
    // or after the content, and the loader below fires only when both have.
    if (self->content_store_ && self->client_session_->asset_stream()) {
      self->client_session_->asset_stream()->set_on_ready(
          [self](const modstream::ModManifest& manifest) {
            // Replace any previous mount (a live reload re-fires this), on the main
            // thread where nothing is reading the Vfs.
            self->vfs_->UnmountByPrefix("modstream:");
            modstream::MountManifest(*self->vfs_, manifest, *self->content_store_);
            RX_INFO("net: mounted {} streamed mod files into the asset vfs", manifest.TotalFiles());
          });
      self->client_session_->asset_stream()->set_on_scripts(
          [self](const std::vector<modstream::ClientScriptEntry>& scripts) {
            if (!self->managed_ || scripts.empty())
              return;
            // Resolve the offer to on-disk cache paths. A file that is not in
            // the cache cannot run (it never streamed); drop it here so the
            // managed world only ever sees loadable paths.
            base::Vector<base::String> paths;
            for (const modstream::ClientScriptEntry& entry : scripts) {
              const auto cached = self->content_store_->PathFor(entry.hash);
              if (!cached) {
                RX_WARN("net: client script '{}' never streamed; skipping it",
                        entry.path.c_str());
                continue;
              }
              paths.push_back(base::String(cached->string().c_str(), cached->string().size()));
            }
            if (paths.empty())
              return;
            const base::String key = ScriptTrust::KeyFor(self->config_.connect_address);
            switch (ScriptOfferActionFor(key)) {
              case ScriptOfferAction::kRun:
                RX_INFO("net: running {} client script(s) from {} (trusted)", paths.size(),
                        key.c_str());
                self->managed_->LoadStreamedScripts(paths);
                break;
              case ScriptOfferAction::kDecline:
                RX_INFO("net: server offered {} client script(s); not running them",
                        paths.size());
                break;
              case ScriptOfferAction::kAsk:
                // Hold the loading screen on the question; the world keeps
                // streaming behind it and the timeout defers meanwhile.
                self->script_consent_.pending = true;
                self->script_consent_.server_key = key;
                self->script_consent_.paths = paths;
                RX_INFO("net: server offered {} client script(s); waiting for the player's "
                        "script-trust decision",
                        paths.size());
                break;
            }
          });
    }
  } else {
    return true;
  }

  self->scheduler_->AddSystem(ecs::Stage::kSim, "net", [self](ecs::World& world, f32 dt) {
    // Clients stream their local character intent before the session ticks, so
    // the server's simulator reads this frame's movement (the intent component
    // is written by the walk controller later in the frame; one frame of age is
    // the accepted cost of the server-simulated model).
    if (self->client_session_ && self->ctx_.walk_mode && self->ctx_.world &&
        self->actors_ && self->actors_->HasPlayer()) {
      const ecs::Entity local = self->actors_->PlayerEntity();
      const auto* intent = world.Get<character::CharacterIntent>(local);
      if (intent) {
        net::PlayerInput input;
        input.move_x = intent->move.x;
        input.move_y = 0.0f;
        input.move_z = intent->move.z;
        if (const auto* state = world.Get<character::CharacterState>(local))
          input.yaw = state->yaw;
        u8 buttons = 0;
        if (intent->jump)
          buttons |= kInputJump;
        if (intent->crouch)
          buttons |= kInputCrouch;
        if (intent->gait == character::CharacterGait::kWalk)
          buttons |= kInputGaitWalk;
        if (intent->gait == character::CharacterGait::kSprint)
          buttons |= kInputGaitSprint;
        input.buttons = buttons;
        self->client_session_->SetInput(input);
      }
    }
    self->session_->Tick(world, dt);
    // Then pull the local body back onto the host's copy of it. After the tick,
    // so the newest snapshot is the one being compared against, and never while
    // the loading screen is up: a client still placing its world has not settled
    // anywhere worth correcting.
    if (NetReconcile.get() && self->client_session_ && self->ctx_.walk_mode &&
        !self->load_screen_up_ && self->actors_ && self->actors_->HasPlayer()) {
      ReconcileLocalPlayer(world, dt, self->client_session_->player_entity(), *self->actors_,
                           &self->reconcile_snapping_);
    }
    // The announcer beats from its own thread and cannot read the session's
    // client map, so the count it publishes is refreshed here, on the thread
    // that owns it.
    if (self->server_session_)
      self->announced_players_.store(self->server_session_->client_count());
    // Apply avatar/vitals offers that outraced their entity's snapshot. The
    // pending maps are tiny (one entry per player), so a scan per tick is free.
    if (self->client_session_ && self->ctx_.world) {
      ecs::World& w = *self->ctx_.world;
      base::Vector<u64> resolved;
      for (auto entry : self->pending_avatars_) {
        const u64 net_id = entry.key;
        const u64 form = entry.value;
        const ecs::Entity e = self->client_session_->replicated_entity(net_id);
        if (e == ecs::kInvalidEntity)
          continue;
        w.Add(e, world::PlayerAvatar{bethesda::GlobalFormId{static_cast<u16>(form >> 32),
                                                            static_cast<u32>(form)}});
        w.Remove<scene::Renderable>(e);  // the actor system renders the body
        resolved.push_back(net_id);
      }
      for (const u64 net_id : resolved)
        self->pending_avatars_.erase(net_id);
      resolved.clear();
      for (auto entry : self->pending_vitals_) {
        const u64 net_id = entry.key;
        const world::PlayerVitals vitals = entry.value;
        const ecs::Entity e = self->client_session_->replicated_entity(net_id);
        if (e == ecs::kInvalidEntity)
          continue;
        w.Add(e, vitals);
        if (vitals.dead)
          w.Add(e, world::Dead{});
        else
          w.Remove<world::Dead>(e);
        resolved.push_back(net_id);
      }
      for (const u64 net_id : resolved)
        self->pending_vitals_.erase(net_id);
    }
  });
  if (self->client_session_) {
    // Remote transforms blend between snapshots. With a renderer that runs
    // per frame; headless clients smooth at the fixed step instead.
    const ecs::Stage stage = self->config_.headless ? ecs::Stage::kPostSim : ecs::Stage::kPreRender;
    self->scheduler_->AddSystem(stage, "net_interpolation", [](ecs::World& world, f32 dt) {
      net::TickInterpolation(world, dt);
    });
  }
  // The managed world booted before the session, so forward any RPC names its
  // mods subscribed to into the live session's registry now.
  if (self->managed_)
    RegisterManagedRpcForwarding(*self);
  return true;
}

void ReloadMods(Engine& engine) {
  Engine* const self = &engine;
  if (self->config_.mods_dir.empty() || !self->server_session_ || !self->mod_catalog_) {
    return;  // not hosting a mods directory; nothing to reload
  }
  base::Optional<modstream::ModCatalog> fresh =
      modstream::ModCatalog::Build(self->config_.mods_dir.c_str());
  if (!fresh) {
    RX_ERROR("net: mod reload failed to catalog '{}', keeping the current set",
             self->config_.mods_dir);
    return;  // a broken edit must not take down the running server
  }
  RX_INFO("net: reloaded mods, now offering {} files ({} bytes)", fresh->manifest().TotalFiles(),
          fresh->manifest().TotalBytes());

  // Point the session at the new catalog before the old one is destroyed, then
  // swap ownership: the new catalog object's address is stable across the move.
  auto next = base::MakeUnique<modstream::ModCatalog>(base::move(*fresh));
  self->server_session_->ReloadCatalog(*next);
  self->mod_catalog_ = base::move(next);

  // Re-mount on the host: drop the old mod providers and mount the new catalog,
  // on the main thread where nothing is reading the Vfs.
  self->vfs_->UnmountByPrefix("modstream:");
  modstream::MountCatalog(*self->vfs_, *self->mod_catalog_);
}

void TickScriptConsent(Engine& engine) {
  Engine* const self = &engine;
  if (!self->script_consent_.pending)
    return;
  if (!self->window_ || !self->managed_) {
    // Nowhere to ask (headless): decline rather than run unasked-for code.
    self->script_consent_.pending = false;
    return;
  }
  // The loading screen is up and holding on the question; read the answer off
  // the raw key state the same frame the player presses it.
  const InputState& keys = self->window_->input();
  const bool run_once = keys.key_pressed(Key::k1);
  const bool always = keys.key_pressed(Key::k2);
  const bool never = keys.key_pressed(Key::k3);
  if (!run_once && !always && !never)
    return;

  const auto& offer = self->script_consent_;
  if (never) {
    ScriptTrust::Remember(offer.server_key, false);
    RX_INFO("net: declined {} client script(s) for this server", offer.paths.size());
  } else {
    if (always)
      ScriptTrust::Remember(offer.server_key, true);
    RX_INFO("net: running {} client script(s) from {}", offer.paths.size(),
            offer.server_key.c_str());
    self->managed_->LoadStreamedScripts(offer.paths);
  }
  self->script_consent_.pending = false;
}
#endif  // RECREATION_HAS_NET

}  // namespace rx

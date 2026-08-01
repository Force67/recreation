#ifndef RECREATION_GAMENET_PROTOCOL_H_
#define RECREATION_GAMENET_PROTOCOL_H_

#include "core/types.h"
#include "net/protocol.h"

namespace rx::net {

// Recreation's game wire version, fed into rx::net::SessionConfig::protocol
// and checked at join. Bump it whenever any payload -- an engine message or
// one of the game codecs below -- changes shape.
// v6 moves the session substrate into rx::net: the engine messages
// (join/snapshot/input/rpc/bubbles) ride rx's hand-rolled wire codec, game
// ids renumbered into the >= kFirstGameMessage range, and the server streams
// per-peer interest bubbles instead of broadcasting. v7 adds replicated door
// lock/open world commands.
inline constexpr u32 kGameProtocolVersion = 7;

// Recreation's application messages. Everything below kFirstGameMessage
// belongs to rx::net (transport ids < 100, engine session ids 101..127);
// these ids surface undecoded through the sessions' game-message sinks.
enum class GameMessage : u16 {
  kQuestUpdate = 128,      // server -> clients: journal deltas (quest_replication)
  kWorldCommands = 129,    // server -> clients: quest-driven world mutations
  kActivateRef = 130,      // client -> server: the player activated a reference
  kActorSync = 131,        // server -> clients: NPC transforms that changed
  kDialogueSelect = 132,   // client -> server: the player chose a dialogue INFO
  kStageRequest = 133,     // client -> server: a debugger stage/objective change
  kObjectiveMarker = 134,  // server -> clients: the active quest objective waypoint
  kAssetManifest = 135,    // server -> client: the mod manifest offered for streaming
  kAssetRequest = 136,     // client -> server: content hashes the client wants
  kAssetReady = 137,       // client -> server: the client finished streaming
  kWarMap = 138,           // server -> clients: the Civil War campaign board
};

static_assert(static_cast<u16>(GameMessage::kQuestUpdate) >= kFirstGameMessage,
              "game messages must stay out of the rx::net engine id range");

}  // namespace rx::net

#endif  // RECREATION_GAMENET_PROTOCOL_H_

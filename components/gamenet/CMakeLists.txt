recreation_add_module(gamenet
  actor_sync.cc
  asset_stream.cc
  objective_marker_net.cc
  quest_replication.cc
  session.cc
  stage_request.cc
  war_map_net.cc
  world_replication.cc
)

# rx::net owns the transport, join handshake, per-peer entity replication and
# the streaming bubbles; this module adds recreation's game layer on top:
# quest/war-map/actor/world-command codecs, dialogue/stage/activate routing
# and mod asset streaming. nanobuf encodes the game payloads, world/quest
# supply the mirrored state, modstream catalogs and caches the streamed mods.
target_link_libraries(recreation_gamenet PUBLIC
  rx::net
  rx::core
  rx::ecs
  recreation::world
  recreation::quest
  recreation::nanobuf
  recreation::modstream
  zetanet
)

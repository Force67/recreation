recreation_add_module(net
  actor_sync.cc
  asset_stream.cc
  objective_marker_net.cc
  quest_replication.cc
  replication.cc
  rpc_channel.cc
  session.cc
  stage_request.cc
  war_map_net.cc
  world_replication.cc
)

# zetanet carries the packets, nanobuf defines what is in them, world/asset
# provide the components the replication layer mirrors, quest supplies the
# journal state the sessions replicate. modstream catalogs and caches the
# streamed mods, rpc supplies the scripting call codec and registry.
target_link_libraries(recreation_net PUBLIC
  rx::core
  rx::ecs
  recreation::world
  recreation::quest
  recreation::nanobuf
  recreation::modstream
  rx::rpc
  zetanet
)

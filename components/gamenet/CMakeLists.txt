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

foreach(t stage_requesttest objective_marker_nettest war_map_nettest
          bubble_nettest activation_nettest)
  recreation_add_test(${t} recreation::gamenet)
endforeach()
recreation_add_test(quest_nettest recreation::gamenet recreation::quest)
recreation_add_test(quest_net_progressiontest recreation::gamenet recreation::quest)
recreation_add_test(world_nettest recreation::gamenet recreation::world)
recreation_add_test(actor_synctest recreation::gamenet recreation::world)
recreation_add_test(asset_streamtest
  recreation::gamenet recreation::modstream rx::asset)
if(TARGET asset_streamtest AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC mis-fires -Warray-bounds inside the fortified memcpy when it cannot
  # range-track std::string growth at -O2. Known compiler bug, not our code
  # (the test is ASan/UBSan clean); silence it on this TU.
  set_source_files_properties(asset_streamtest.cc
    PROPERTIES COMPILE_OPTIONS -Wno-array-bounds)
endif()

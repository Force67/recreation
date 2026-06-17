add_executable(pexdump main.cc)
target_link_libraries(pexdump PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexdump)

add_executable(pexrun run.cc)
target_link_libraries(pexrun PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexrun)

add_executable(vmaddump vmad.cc)
target_link_libraries(vmaddump PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(vmaddump)

add_executable(scripttest scripttest.cc)
target_link_libraries(scripttest PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(scripttest)

add_executable(nativescan nativescan.cc)
target_link_libraries(nativescan PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(nativescan)

add_executable(bindingstest bindingstest.cc)
target_link_libraries(bindingstest PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(bindingstest)

add_executable(questtest questtest.cc)
target_link_libraries(questtest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(questtest)

add_executable(scenetest scenetest.cc)
target_link_libraries(scenetest PRIVATE recreation::quest)
recreation_set_warnings(scenetest)

add_executable(conditiontest conditiontest.cc)
target_link_libraries(conditiontest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(conditiontest)

add_executable(questgraphtest questgraphtest.cc)
target_link_libraries(questgraphtest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(questgraphtest)

add_executable(questimporttest questimporttest.cc)
target_link_libraries(questimporttest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(questimporttest)

add_executable(interactiontest interactiontest.cc)
target_link_libraries(interactiontest PRIVATE recreation::world)
recreation_set_warnings(interactiontest)

add_executable(npc_aitest npc_aitest.cc)
target_link_libraries(npc_aitest PRIVATE recreation::world)
recreation_set_warnings(npc_aitest)

add_executable(objective_markertest objective_markertest.cc)
target_link_libraries(objective_markertest PRIVATE recreation::world)
recreation_set_warnings(objective_markertest)

add_executable(steering_avoidancetest steering_avoidancetest.cc)
target_link_libraries(steering_avoidancetest PRIVATE recreation::world)
recreation_set_warnings(steering_avoidancetest)

add_executable(pathfindtest pathfindtest.cc)
target_link_libraries(pathfindtest PRIVATE recreation::world)
recreation_set_warnings(pathfindtest)

add_executable(quest_worldtest quest_worldtest.cc)
target_link_libraries(quest_worldtest PRIVATE recreation::world)
recreation_set_warnings(quest_worldtest)

add_executable(replicamodetest replicamodetest.cc)
target_link_libraries(replicamodetest PRIVATE recreation::script recreation::bethesda)
recreation_set_warnings(replicamodetest)

add_executable(dialoguetest dialoguetest.cc)
target_link_libraries(dialoguetest PRIVATE recreation::dialogue recreation::bethesda)
recreation_set_warnings(dialoguetest)

# Quest replication lives in the net module, so this only builds with networking.
if(RECREATION_NET)
  add_executable(quest_nettest quest_nettest.cc)
  target_link_libraries(quest_nettest PRIVATE recreation::net recreation::quest)
  recreation_set_warnings(quest_nettest)

  add_executable(world_nettest world_nettest.cc)
  target_link_libraries(world_nettest PRIVATE recreation::net recreation::world)
  recreation_set_warnings(world_nettest)

  add_executable(actor_synctest actor_synctest.cc)
  target_link_libraries(actor_synctest PRIVATE recreation::net recreation::world)
  recreation_set_warnings(actor_synctest)

  add_executable(stage_requesttest stage_requesttest.cc)
  target_link_libraries(stage_requesttest PRIVATE recreation::net)
  recreation_set_warnings(stage_requesttest)

  add_executable(objective_marker_nettest objective_marker_nettest.cc)
  target_link_libraries(objective_marker_nettest PRIVATE recreation::net)
  recreation_set_warnings(objective_marker_nettest)
endif()

add_executable(pexsoak soak.cc)
target_link_libraries(pexsoak PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexsoak)

add_executable(pexfuzz fuzz.cc)
target_link_libraries(pexfuzz PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexfuzz)

# Deterministic VM regression suite (no game data needed), so `ctest` is the
# one-command gate. The corpus checks (pexsoak/pexfuzz/nativescan) take the
# Skyrim data dir and run separately.
add_test(NAME papyrus_selftest COMMAND pexrun selftest)
add_test(NAME papyrus_skyrimtest COMMAND pexrun skyrimtest)
add_test(NAME papyrus_guesttest COMMAND pexrun guesttest)
add_test(NAME papyrus_conctest COMMAND pexrun conctest)
add_test(NAME papyrus_separationtest COMMAND pexrun separationtest)
add_test(NAME papyrus_tracetest COMMAND pexrun tracetest)
add_test(NAME quest_systemtest COMMAND questtest)
add_test(NAME scenetest COMMAND scenetest)
add_test(NAME conditiontest COMMAND conditiontest)
add_test(NAME questgraphtest COMMAND questgraphtest)
add_test(NAME questimporttest COMMAND questimporttest)
add_test(NAME interactiontest COMMAND interactiontest)
add_test(NAME npc_aitest COMMAND npc_aitest)
add_test(NAME objective_markertest COMMAND objective_markertest)
add_test(NAME steering_avoidancetest COMMAND steering_avoidancetest)
add_test(NAME pathfindtest COMMAND pathfindtest)
add_test(NAME quest_worldtest COMMAND quest_worldtest)
add_test(NAME replicamodetest COMMAND replicamodetest)
add_test(NAME dialoguetest COMMAND dialoguetest)
if(RECREATION_NET)
  add_test(NAME quest_nettest COMMAND quest_nettest)
  add_test(NAME world_nettest COMMAND world_nettest)
  add_test(NAME actor_synctest COMMAND actor_synctest)
  add_test(NAME stage_requesttest COMMAND stage_requesttest)
  add_test(NAME objective_marker_nettest COMMAND objective_marker_nettest)
endif()

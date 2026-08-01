recreation_add_module(quest
  condition.cc
  ctda.cc
  package_record.cc
  quest_def.cc
  quest_graph.cc
  quest_import.cc
  quest_system.cc
  scene.cc
  scene_record.cc
  scene_compile.cc
  scene_player.cc
  scene_runtime.cc
)

# State is core-only; the definition parser reads QUST records via bethesda.
target_link_libraries(recreation_quest PUBLIC
  rx::core
  recreation::bethesda)

recreation_add_test(questtest AS quest_systemtest recreation::quest recreation::bethesda)
foreach(t conditiontest questgraphtest questimporttest package_recordtest
          quest_aliastest scene_recordtest)
  recreation_add_test(${t} recreation::quest recreation::bethesda)
endforeach()
foreach(t scenetest scene_compiletest scene_playertest scene_runtimetest)
  recreation_add_test(${t} recreation::quest)
endforeach()

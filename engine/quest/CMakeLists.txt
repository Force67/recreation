recreation_add_module(quest
  condition.cc
  ctda.cc
  quest_def.cc
  quest_graph.cc
  quest_import.cc
  quest_system.cc
  scene.cc
  scene_record.cc
)

# State is core-only; the definition parser reads QUST records via bethesda.
target_link_libraries(recreation_quest PUBLIC
  recreation::core
  recreation::bethesda)

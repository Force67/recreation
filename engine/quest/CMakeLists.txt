recreation_add_module(quest
  quest_def.cc
  quest_system.cc
)

# State is core-only; the definition parser reads QUST records via bethesda.
target_link_libraries(recreation_quest PUBLIC
  recreation::core
  recreation::bethesda)

recreation_add_module(world
  carriage_records.cc
  carriage_rig.cc
  cell_streaming.cc
  cine_camera.cc
  grass_baker.cc
  land_baker.cc
  planet_tile.cc
  prop_streaming.cc
  quest_world.cc
  navgrid.cc
  terrain_edits.cc
)

target_link_libraries(recreation_world PUBLIC rx::physics)
target_link_libraries(recreation_world PUBLIC rx::render)
target_link_libraries(recreation_world PUBLIC
  rx::core rx::ecs rx::asset recreation::bethesda recreation::quest)

foreach(t cine_cameratest interactiontest npc_aitest objective_markertest
          steering_avoidancetest pathfindtest carriage_rigtest quest_worldtest
          navgridtest)
  recreation_add_test(${t} recreation::world)
endforeach()
recreation_add_test(prop_streamingtest
  recreation::world recreation::script recreation::bethesda)
recreation_add_test(carriage_recordstest recreation::world recreation::bethesda)
recreation_add_test(combattest
  recreation::script recreation::world recreation::quest)

# Terrain edits are pure height/vertex maths, so the test compiles the unit
# directly rather than dragging in the module's physics and render deps.
recreation_add_test(terrain_edittest SRCS terrain_edits.cc)
if(TARGET terrain_edittest)
  target_include_directories(terrain_edittest PRIVATE ${RECREATION_RX_DIR}/engine)
  target_link_libraries(terrain_edittest PRIVATE equilibrium::base)
endif()

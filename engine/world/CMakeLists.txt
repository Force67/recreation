recreation_add_module(world
  carriage_rig.cc
  cell_streaming.cc
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
  rx::core rx::ecs rx::asset recreation::bethesda)

recreation_add_module(world
  cell_streaming.cc
  grass_baker.cc
  land_baker.cc
  quest_world.cc
  navgrid.cc
)

target_link_libraries(recreation_world PUBLIC recreation::physics)
target_link_libraries(recreation_world PUBLIC
  recreation::core recreation::ecs recreation::asset recreation::bethesda)

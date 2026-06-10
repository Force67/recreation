recreation_add_module(world
  cell_streaming.cc
)

target_link_libraries(recreation_world PUBLIC
  recreation::core recreation::ecs recreation::asset recreation::bethesda)

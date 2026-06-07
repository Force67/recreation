recreation_add_module(world
  src/cell_streaming.cc
)

target_link_libraries(recreation_world PUBLIC
  recreation::core recreation::ecs recreation::asset recreation::bethesda)

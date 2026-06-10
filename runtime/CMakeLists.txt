add_executable(recreation
  main.cc
  engine.cc
)

target_link_libraries(recreation PRIVATE
  recreation::core
  recreation::ecs
  recreation::asset
  recreation::render
  recreation::bethesda
  recreation::world
  recreation::net
  recreation::script
)
recreation_set_warnings(recreation)

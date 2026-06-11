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

# Dedicated server: same engine, headless config baked in. Still links the
# renderer module since the engine owns one; it never initializes vulkan.
add_executable(recreation-server
  server_main.cc
  engine.cc
)

target_link_libraries(recreation-server PRIVATE
  recreation::core
  recreation::ecs
  recreation::asset
  recreation::render
  recreation::bethesda
  recreation::world
  recreation::net
  recreation::script
)
recreation_set_warnings(recreation-server)

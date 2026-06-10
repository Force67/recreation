recreation_add_module(ecs
  archetype.cc
  world.cc
  scheduler.cc
)

target_link_libraries(recreation_ecs PUBLIC recreation::core)

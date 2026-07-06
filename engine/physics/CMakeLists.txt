if(RECREATION_JOLT)
  recreation_add_module(physics
    physics_world.cc
    kinema_jolt.cc
  )
  target_link_libraries(recreation_physics PUBLIC recreation::core recreation::asset)
  target_link_libraries(recreation_physics PRIVATE Jolt kinema::kinema)
else()
  recreation_add_module(physics
    physics_world_stub.cc
  )
  target_link_libraries(recreation_physics PUBLIC recreation::core recreation::asset)
endif()

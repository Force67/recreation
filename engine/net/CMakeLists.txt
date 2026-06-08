recreation_add_module(net
  src/transport.cc
  src/replication.cc
  src/session.cc
)

target_link_libraries(recreation_net PUBLIC recreation::core recreation::ecs)

if(WIN32)
  target_link_libraries(recreation_net PUBLIC ws2_32)
endif()

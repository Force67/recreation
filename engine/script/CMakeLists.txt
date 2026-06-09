recreation_add_module(script
  src/papyrus_vm.cc
)

target_link_libraries(recreation_script PUBLIC
  recreation::core recreation::ecs recreation::bethesda)

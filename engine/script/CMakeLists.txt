recreation_add_module(script
  papyrus/opcode.cc
  papyrus/pex.cc
  papyrus_vm.cc
)

target_link_libraries(recreation_script PUBLIC
  recreation::core recreation::ecs recreation::bethesda)

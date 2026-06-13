recreation_add_module(script
  papyrus/opcode.cc
  papyrus/pex.cc
  papyrus/value.cc
  papyrus/interpreter.cc
  papyrus/native.cc
  papyrus/vm.cc
  papyrus_guest.cc
)

target_link_libraries(recreation_script PUBLIC
  recreation::core recreation::ecs recreation::bethesda)

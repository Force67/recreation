recreation_add_module(script
  papyrus/opcode.cc
  papyrus/pex.cc
  papyrus/value.cc
  papyrus/interpreter.cc
  papyrus/native.cc
  papyrus/vm.cc
  papyrus_guest.cc
  host/clr_host.cc
  host/guest_bridge.cc
  games/skyrim/skyrim_natives.cc
)

# The CoreCLR host dlopens libhostfxr at runtime (no build-time .NET needed),
# so it only requires the dynamic loader.
target_link_libraries(recreation_script PUBLIC
  recreation::core recreation::ecs recreation::bethesda ${CMAKE_DL_LIBS})

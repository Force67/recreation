recreation_add_module(script
  obscript/obscript.cc
  obscript/obscript_runtime.cc
  papyrus/opcode.cc
  papyrus/pex.cc
  papyrus/value.cc
  papyrus/interpreter.cc
  papyrus/native.cc
  papyrus/fiber.cc
  papyrus/minicoro_impl.cc
  papyrus/fiber_scheduler.cc
  papyrus/decompiler.cc
  papyrus/transpile.cc
  papyrus/vm.cc
  papyrus_guest.cc
  script_system.cc
  host/clr_host.cc
  host/guest_bridge.cc
  host/managed_gc_profile.cc
  host/managed_host.cc
  games/skyrim/skyrim_natives.cc
  games/skyrim/skyrim_native_state.cc
  games/skyrim/skyrim_natives_game_env.cc
  games/skyrim/skyrim_natives_utility.cc
  games/skyrim/skyrim_natives_actorref.cc
  games/skyrim/skyrim_natives_actor.cc
  games/skyrim/skyrim_natives_objectref.cc
  games/skyrim/skyrim_natives_game.cc
  games/skyrim/skyrim_natives_debug.cc
  games/skyrim/skyrim_natives_magiceffect.cc
  games/skyrim/skyrim_natives_faction.cc
  games/skyrim/skyrim_natives_alias.cc
  games/skyrim/skyrim_natives_form.cc
  games/skyrim/skyrim_natives_world.cc
  games/skyrim/skyrim_natives_av.cc
  games/skyrim/skyrim_natives_items.cc
  games/skyrim/skyrim_natives_utility2.cc
  games/skyrim/skyrim_bindings.cc
  games/skyrim/skyrim_bindings_audio.cc
  games/skyrim/skyrim_bindings_item_data.cc
  games/skyrim/skyrim_bindings_crafting.cc
  games/skyrim/skyrim_bindings_leveled.cc
  games/skyrim/skyrim_bindings_faction.cc
  games/skyrim/skyrim_condition_context.cc
)

# The CoreCLR host dlopens libhostfxr at runtime (no build-time .NET needed),
# so it only requires the dynamic loader.
target_link_libraries(recreation_script PUBLIC
  rx::core rx::ecs recreation::bethesda recreation::quest recreation::audio
  ${CMAKE_DL_LIBS})

# minicoro backs the Papyrus fiber (latent Wait). Header-only; its implementation
# is compiled once in minicoro_impl.cc with warnings off (third-party code).
target_include_directories(recreation_script PRIVATE ${CMAKE_SOURCE_DIR}/third_party/minicoro)
set_source_files_properties(papyrus/minicoro_impl.cc PROPERTIES
  COMPILE_OPTIONS $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)

recreation_add_module(core
  log.cc
  job_system.cc
  frame_timer.cc
  world_clock.cc
  window.cc
  input_bindings.cc
)

# equilibrium's base library; linking it here makes base:: containers and
# strings available to every engine module.
target_link_libraries(recreation_core PUBLIC
  equilibrium::base
  $<$<PLATFORM_ID:Linux>:pthread>)

if(SDL3_FOUND)
  target_sources(recreation_core PRIVATE window_sdl3.cc)
  target_link_libraries(recreation_core PUBLIC SDL3::SDL3)
  target_compile_definitions(recreation_core PRIVATE RECREATION_HAS_SDL3=1)
endif()

if(ANDROID)
  target_sources(recreation_core PRIVATE window_android.cc)
  target_link_libraries(recreation_core PUBLIC volk log android)
endif()

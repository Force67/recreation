recreation_add_module(core
  log.cc
  job_system.cc
  frame_timer.cc
  window.cc
)

# znet_base is equilibrium's base library; linking it here makes base::
# containers and strings available to every engine module.
target_link_libraries(recreation_core PUBLIC
  znet_base
  $<$<PLATFORM_ID:Linux>:pthread>)

if(SDL3_FOUND)
  target_sources(recreation_core PRIVATE window_sdl3.cc)
  target_link_libraries(recreation_core PUBLIC SDL3::SDL3)
  target_compile_definitions(recreation_core PRIVATE RECREATION_HAS_SDL3=1)
endif()

if(ANDROID)
  target_link_libraries(recreation_core PUBLIC log android)
endif()

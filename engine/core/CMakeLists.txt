recreation_add_module(core
  log.cc
  job_system.cc
  frame_timer.cc
  window.cc
)

target_link_libraries(recreation_core PUBLIC $<$<PLATFORM_ID:Linux>:pthread>)

if(ANDROID)
  target_link_libraries(recreation_core PUBLIC log android)
endif()

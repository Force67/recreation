recreation_add_module(audio
  audio_clip.cc
  wav.cc
  mixer.cc
  audio_device.cc
  audio_system.cc
)

# Sound bytes come from the game Vfs (loose files + BSA/BA2); the mixer math and
# decoders only need core. The SDL output backend is optional: without it (the
# Android native backend, a headless server) audio_device.cc compiles to a silent
# stub and the rest of the module is unchanged.
target_link_libraries(recreation_audio PUBLIC recreation::core recreation::asset)

if(SDL3_FOUND)
  target_link_libraries(recreation_audio PUBLIC SDL3::SDL3)
  target_compile_definitions(recreation_audio PRIVATE RECREATION_HAS_SDL3_AUDIO=1)
endif()

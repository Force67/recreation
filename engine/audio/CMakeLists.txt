# Bethesda sound records (SOUN/SNDR/REGN) and the ambient layer stay in
# recreation; the generic mixer/decoders/output backend now come from rx::audio.
recreation_add_module(audio
  sound_catalog.cc
  ambient.cc
)

target_link_libraries(recreation_audio PUBLIC
  rx::audio recreation::bethesda)

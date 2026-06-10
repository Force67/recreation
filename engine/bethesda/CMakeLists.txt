recreation_add_module(bethesda
  game_profile.cc
  plugin.cc
  load_order.cc
  bsa.cc
  ba2.cc
  nif.cc
  converters.cc
  strings.cc
)

target_link_libraries(recreation_bethesda PUBLIC recreation::core recreation::asset)

if(ZLIB_FOUND)
  target_link_libraries(recreation_bethesda PRIVATE ZLIB::ZLIB)
  target_compile_definitions(recreation_bethesda PRIVATE RECREATION_HAS_ZLIB=1)
endif()

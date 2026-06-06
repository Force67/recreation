recreation_add_module(bethesda
  src/game_profile.cc
  src/plugin.cc
  src/load_order.cc
)

target_link_libraries(recreation_bethesda PUBLIC recreation::core recreation::asset)

if(ZLIB_FOUND)
  target_link_libraries(recreation_bethesda PRIVATE ZLIB::ZLIB)
  target_compile_definitions(recreation_bethesda PRIVATE RECREATION_HAS_ZLIB=1)
endif()

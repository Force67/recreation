recreation_add_module(bethesda
  game_profile.cc
  plugin.cc
  load_order.cc
  compression.cc
  bsa.cc
  ba2.cc
  nif.cc
  converters.cc
  strings.cc
)

target_link_libraries(recreation_bethesda PUBLIC recreation::core recreation::asset)

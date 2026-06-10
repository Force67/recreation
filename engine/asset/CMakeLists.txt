recreation_add_module(asset
  vfs.cc
  asset_database.cc
)

target_link_libraries(recreation_asset PUBLIC recreation::core)

recreation_add_module(asset
  vfs.cc
  asset_database.cc
  primitives.cc
  simplify.cc
  subdivide.cc
  gltf_loader.cc
  materialx.cc
)

target_link_libraries(recreation_asset PUBLIC recreation::core)
target_link_libraries(recreation_asset PRIVATE recreation::cgltf recreation::stb)

if(NOT MSVC)
  # vendored single header libraries are not warning clean
  set_source_files_properties(gltf_loader.cc PROPERTIES COMPILE_OPTIONS -Wno-shadow)
endif()

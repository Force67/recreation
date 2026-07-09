add_executable(assetdump main.cc)
target_link_libraries(assetdump PRIVATE recreation::bethesda rx::asset)
recreation_set_warnings(assetdump)

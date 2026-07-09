add_executable(esminfo main.cc)
target_link_libraries(esminfo PRIVATE recreation::bethesda)
recreation_set_warnings(esminfo)

# assetdump lives next door; registered here because the top level only pulls
# in this tools directory.
add_executable(assetdump ../assetdump/main.cc)
target_link_libraries(assetdump PRIVATE recreation::bethesda rx::asset)
recreation_set_warnings(assetdump)

add_executable(hkxinfo ../hkxinfo/main.cc)
target_link_libraries(hkxinfo PRIVATE recreation::bethesda rx::asset)
recreation_set_warnings(hkxinfo)

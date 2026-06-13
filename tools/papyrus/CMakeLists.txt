add_executable(pexdump main.cc)
target_link_libraries(pexdump PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexdump)

add_executable(pexrun run.cc)
target_link_libraries(pexrun PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexrun)

add_executable(vmaddump vmad.cc)
target_link_libraries(vmaddump PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(vmaddump)

add_executable(scripttest scripttest.cc)
target_link_libraries(scripttest PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(scripttest)

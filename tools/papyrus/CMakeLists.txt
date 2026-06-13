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

add_executable(nativescan nativescan.cc)
target_link_libraries(nativescan PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(nativescan)

add_executable(bindingstest bindingstest.cc)
target_link_libraries(bindingstest PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(bindingstest)

add_executable(pexsoak soak.cc)
target_link_libraries(pexsoak PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexsoak)

add_executable(pexfuzz fuzz.cc)
target_link_libraries(pexfuzz PRIVATE recreation::script recreation::bethesda recreation::asset)
recreation_set_warnings(pexfuzz)

# Deterministic VM regression suite (no game data needed), so `ctest` is the
# one-command gate. The corpus checks (pexsoak/pexfuzz/nativescan) take the
# Skyrim data dir and run separately.
add_test(NAME papyrus_selftest COMMAND pexrun selftest)
add_test(NAME papyrus_skyrimtest COMMAND pexrun skyrimtest)
add_test(NAME papyrus_guesttest COMMAND pexrun guesttest)
add_test(NAME papyrus_conctest COMMAND pexrun conctest)
add_test(NAME papyrus_separationtest COMMAND pexrun separationtest)

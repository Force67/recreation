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

add_executable(questtest questtest.cc)
target_link_libraries(questtest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(questtest)

add_executable(conditiontest conditiontest.cc)
target_link_libraries(conditiontest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(conditiontest)

add_executable(interactiontest interactiontest.cc)
target_link_libraries(interactiontest PRIVATE recreation::world)
recreation_set_warnings(interactiontest)

add_executable(dialoguetest dialoguetest.cc)
target_link_libraries(dialoguetest PRIVATE recreation::dialogue recreation::bethesda)
recreation_set_warnings(dialoguetest)

# Quest replication lives in the net module, so this only builds with networking.
if(RECREATION_NET)
  add_executable(quest_nettest quest_nettest.cc)
  target_link_libraries(quest_nettest PRIVATE recreation::net recreation::quest)
  recreation_set_warnings(quest_nettest)
endif()

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
add_test(NAME papyrus_tracetest COMMAND pexrun tracetest)
add_test(NAME quest_systemtest COMMAND questtest)
add_test(NAME conditiontest COMMAND conditiontest)
add_test(NAME interactiontest COMMAND interactiontest)
add_test(NAME dialoguetest COMMAND dialoguetest)
if(RECREATION_NET)
  add_test(NAME quest_nettest COMMAND quest_nettest)
endif()

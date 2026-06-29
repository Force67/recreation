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

add_executable(proximitytest proximitytest.cc)
target_link_libraries(proximitytest PRIVATE recreation::script)
recreation_set_warnings(proximitytest)

add_executable(daycycletest daycycletest.cc)
target_link_libraries(daycycletest PRIVATE recreation::script recreation::core)
recreation_set_warnings(daycycletest)

add_executable(featuretest featuretest.cc)
target_link_libraries(featuretest PRIVATE recreation::core)
recreation_set_warnings(featuretest)

add_executable(optiontest optiontest.cc)
target_link_libraries(optiontest PRIVATE equilibrium::base)
recreation_set_warnings(optiontest)

add_executable(skytest skytest.cc)
recreation_set_warnings(skytest)

add_executable(weathertest weathertest.cc)
target_link_libraries(weathertest PRIVATE recreation::weather)
recreation_set_warnings(weathertest)

add_executable(audiotest audiotest.cc)
target_link_libraries(audiotest PRIVATE recreation::audio)
recreation_set_warnings(audiotest)

add_executable(trailertest trailertest.cc ${CMAKE_SOURCE_DIR}/runtime/trailer.cc)
target_include_directories(trailertest PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(trailertest PRIVATE recreation::weather recreation::core)
recreation_set_warnings(trailertest)

add_executable(questtest questtest.cc)
target_link_libraries(questtest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(questtest)

add_executable(scenetest scenetest.cc)
target_link_libraries(scenetest PRIVATE recreation::quest)
recreation_set_warnings(scenetest)

add_executable(scene_recordtest scene_recordtest.cc)
target_link_libraries(scene_recordtest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(scene_recordtest)

add_executable(scene_compiletest scene_compiletest.cc)
target_link_libraries(scene_compiletest PRIVATE recreation::quest)
recreation_set_warnings(scene_compiletest)

add_executable(scene_playertest scene_playertest.cc)
target_link_libraries(scene_playertest PRIVATE recreation::quest)
recreation_set_warnings(scene_playertest)

add_executable(conditiontest conditiontest.cc)
target_link_libraries(conditiontest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(conditiontest)

add_executable(ba2test ba2test.cc)
target_link_libraries(ba2test PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(ba2test)

add_executable(modstreamtest modstreamtest.cc)
target_link_libraries(modstreamtest PRIVATE recreation::modstream recreation::asset)
recreation_set_warnings(modstreamtest)

add_executable(rpctest rpctest.cc)
target_link_libraries(rpctest PRIVATE recreation::rpc)
recreation_set_warnings(rpctest)

add_executable(codec_fuzztest codec_fuzztest.cc)
target_link_libraries(codec_fuzztest PRIVATE recreation::modstream recreation::rpc)
recreation_set_warnings(codec_fuzztest)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 15 mis-fires -Wfree-nonheap-object when it inlines std::variant's
  # copy-assignment (the string/blob alternatives) at -O2. Known compiler bug,
  # not our code; silence it on this TU which copies RpcValue heavily.
  set_source_files_properties(rpctest.cc PROPERTIES COMPILE_OPTIONS -Wno-free-nonheap-object)
endif()

add_executable(pexfo4test pexfo4test.cc)
target_link_libraries(pexfo4test PRIVATE recreation::script)
recreation_set_warnings(pexfo4test)

add_executable(vmad_fo4test vmad_fo4test.cc)
target_link_libraries(vmad_fo4test PRIVATE recreation::bethesda)
recreation_set_warnings(vmad_fo4test)

add_executable(nif_fo4test nif_fo4test.cc)
target_link_libraries(nif_fo4test PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(nif_fo4test)

add_executable(nif_starfieldtest nif_starfieldtest.cc)
target_link_libraries(nif_starfieldtest PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(nif_starfieldtest)

add_executable(starfield_skinned_meshtest starfield_skinned_meshtest.cc)
target_link_libraries(starfield_skinned_meshtest PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(starfield_skinned_meshtest)

add_executable(material_dbtest material_dbtest.cc)
target_link_libraries(material_dbtest PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(material_dbtest)

add_executable(bgsm_fo4test bgsm_fo4test.cc)
target_link_libraries(bgsm_fo4test PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(bgsm_fo4test)

add_executable(dds_srgb_fo4test dds_srgb_fo4test.cc)
target_link_libraries(dds_srgb_fo4test PRIVATE recreation::bethesda recreation::asset)
recreation_set_warnings(dds_srgb_fo4test)

add_executable(questgraphtest questgraphtest.cc)
target_link_libraries(questgraphtest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(questgraphtest)

add_executable(questimporttest questimporttest.cc)
target_link_libraries(questimporttest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(questimporttest)
add_executable(package_recordtest package_recordtest.cc)
target_link_libraries(package_recordtest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(package_recordtest)

add_executable(interactiontest interactiontest.cc)
target_link_libraries(interactiontest PRIVATE recreation::world)
recreation_set_warnings(interactiontest)

add_executable(npc_aitest npc_aitest.cc)
target_link_libraries(npc_aitest PRIVATE recreation::world)
recreation_set_warnings(npc_aitest)

add_executable(objective_markertest objective_markertest.cc)
target_link_libraries(objective_markertest PRIVATE recreation::world)
recreation_set_warnings(objective_markertest)

add_executable(combattest combattest.cc)
target_link_libraries(combattest PRIVATE recreation::script recreation::world recreation::quest)
recreation_set_warnings(combattest)

add_executable(alias_handletest alias_handletest.cc)
target_link_libraries(alias_handletest PRIVATE recreation::script)
recreation_set_warnings(alias_handletest)

add_executable(quest_aliastest quest_aliastest.cc)
target_link_libraries(quest_aliastest PRIVATE recreation::quest recreation::bethesda)
recreation_set_warnings(quest_aliastest)

add_executable(steering_avoidancetest steering_avoidancetest.cc)
target_link_libraries(steering_avoidancetest PRIVATE recreation::world)
recreation_set_warnings(steering_avoidancetest)

add_executable(pathfindtest pathfindtest.cc)
target_link_libraries(pathfindtest PRIVATE recreation::world)
recreation_set_warnings(pathfindtest)

add_executable(quest_worldtest quest_worldtest.cc)
target_link_libraries(quest_worldtest PRIVATE recreation::world)
recreation_set_warnings(quest_worldtest)

add_executable(replicamodetest replicamodetest.cc)
target_link_libraries(replicamodetest PRIVATE recreation::script recreation::bethesda)
recreation_set_warnings(replicamodetest)
add_executable(navgridtest navgridtest.cc)
target_link_libraries(navgridtest PRIVATE recreation::world)
recreation_set_warnings(navgridtest)

add_executable(dialoguetest dialoguetest.cc)
target_link_libraries(dialoguetest PRIVATE recreation::dialogue recreation::bethesda)
recreation_set_warnings(dialoguetest)

# Quest replication lives in the net module, so this only builds with networking.
if(RECREATION_NET)
  add_executable(quest_nettest quest_nettest.cc)
  target_link_libraries(quest_nettest PRIVATE recreation::net recreation::quest)
  recreation_set_warnings(quest_nettest)

  add_executable(world_nettest world_nettest.cc)
  target_link_libraries(world_nettest PRIVATE recreation::net recreation::world)
  recreation_set_warnings(world_nettest)

  add_executable(actor_synctest actor_synctest.cc)
  target_link_libraries(actor_synctest PRIVATE recreation::net recreation::world)
  recreation_set_warnings(actor_synctest)

  add_executable(stage_requesttest stage_requesttest.cc)
  target_link_libraries(stage_requesttest PRIVATE recreation::net)
  recreation_set_warnings(stage_requesttest)

  add_executable(asset_streamtest asset_streamtest.cc)
  target_link_libraries(asset_streamtest PRIVATE recreation::net recreation::modstream recreation::asset)
  recreation_set_warnings(asset_streamtest)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC mis-fires -Warray-bounds inside the fortified memcpy when it cannot
    # range-track std::string growth at -O2. Known compiler bug, not our code
    # (the test is ASan/UBSan clean); silence it on this TU.
    set_source_files_properties(asset_streamtest.cc PROPERTIES COMPILE_OPTIONS -Wno-array-bounds)
  endif()

  add_executable(objective_marker_nettest objective_marker_nettest.cc)
  target_link_libraries(objective_marker_nettest PRIVATE recreation::net)
  recreation_set_warnings(objective_marker_nettest)

  add_executable(war_map_nettest war_map_nettest.cc)
  target_link_libraries(war_map_nettest PRIVATE recreation::net)
  recreation_set_warnings(war_map_nettest)

  add_executable(quest_net_progressiontest quest_net_progressiontest.cc)
  target_link_libraries(quest_net_progressiontest PRIVATE recreation::net recreation::quest)
  recreation_set_warnings(quest_net_progressiontest)
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
add_test(NAME scenetest COMMAND scenetest)
add_test(NAME scene_recordtest COMMAND scene_recordtest)
add_test(NAME scene_compiletest COMMAND scene_compiletest)
add_test(NAME scene_playertest COMMAND scene_playertest)
add_test(NAME conditiontest COMMAND conditiontest)
add_test(NAME ba2test COMMAND ba2test)
add_test(NAME modstreamtest COMMAND modstreamtest)
add_test(NAME rpctest COMMAND rpctest)
add_test(NAME codec_fuzztest COMMAND codec_fuzztest)
add_test(NAME pexfo4test COMMAND pexfo4test)
add_test(NAME vmad_fo4test COMMAND vmad_fo4test)
add_test(NAME nif_fo4test COMMAND nif_fo4test)
add_test(NAME nif_starfieldtest COMMAND nif_starfieldtest)
add_test(NAME starfield_skinned_meshtest COMMAND starfield_skinned_meshtest)
add_test(NAME material_dbtest COMMAND material_dbtest)
add_test(NAME bgsm_fo4test COMMAND bgsm_fo4test)
add_test(NAME dds_srgb_fo4test COMMAND dds_srgb_fo4test)
add_test(NAME questgraphtest COMMAND questgraphtest)
add_test(NAME questimporttest COMMAND questimporttest)
add_test(NAME package_recordtest COMMAND package_recordtest)
add_test(NAME interactiontest COMMAND interactiontest)
add_test(NAME npc_aitest COMMAND npc_aitest)
add_test(NAME objective_markertest COMMAND objective_markertest)
add_test(NAME combattest COMMAND combattest)
add_test(NAME alias_handletest COMMAND alias_handletest)
add_test(NAME proximitytest COMMAND proximitytest)
add_test(NAME daycycletest COMMAND daycycletest)
add_test(NAME featuretest COMMAND featuretest)
add_test(NAME optiontest COMMAND optiontest)
add_test(NAME skytest COMMAND skytest)
add_test(NAME weathertest COMMAND weathertest)
add_test(NAME audiotest COMMAND audiotest)
add_test(NAME trailertest COMMAND trailertest)
add_test(NAME quest_aliastest COMMAND quest_aliastest)
add_test(NAME steering_avoidancetest COMMAND steering_avoidancetest)
add_test(NAME pathfindtest COMMAND pathfindtest)
add_test(NAME quest_worldtest COMMAND quest_worldtest)
add_test(NAME replicamodetest COMMAND replicamodetest)
add_test(NAME navgridtest COMMAND navgridtest)
add_test(NAME dialoguetest COMMAND dialoguetest)
if(RECREATION_NET)
  add_test(NAME quest_nettest COMMAND quest_nettest)
  add_test(NAME world_nettest COMMAND world_nettest)
  add_test(NAME actor_synctest COMMAND actor_synctest)
  add_test(NAME stage_requesttest COMMAND stage_requesttest)
  add_test(NAME objective_marker_nettest COMMAND objective_marker_nettest)
  add_test(NAME war_map_nettest COMMAND war_map_nettest)
  add_test(NAME quest_net_progressiontest COMMAND quest_net_progressiontest)
  add_test(NAME asset_streamtest COMMAND asset_streamtest)
endif()

# Managed (C#) scripting gates, wired only when a .NET SDK is on the configure
# PATH. A setup fixture builds the assembly and its test runner once; the unit
# tests cover the pure-managed modding framework, and hosttest drives the full
# C#<->engine dispatch path through the real bridge (it skips cleanly without a
# .NET runtime at test time).
find_program(DOTNET_EXE dotnet)
if(DOTNET_EXE)
  set(MANAGED_DIR ${CMAKE_SOURCE_DIR}/sdk)
  set(MANAGED_OUT ${CMAKE_CURRENT_BINARY_DIR}/managed)
  set(MANAGED_DLL ${MANAGED_OUT}/Recreation.Scripting.dll)
  set(MANAGED_CFG ${MANAGED_OUT}/Recreation.Scripting.runtimeconfig.json)
  set(MANAGED_TESTS ${MANAGED_OUT}/Recreation.Scripting.Tests.dll)

  add_test(NAME managed_build
    COMMAND ${DOTNET_EXE} build ${MANAGED_DIR}/tests/Recreation.Scripting.Tests.csproj
            -c Release -o ${MANAGED_OUT} --nologo)
  set_tests_properties(managed_build PROPERTIES FIXTURES_SETUP managed_fx)

  add_test(NAME managed_scripting_tests COMMAND ${DOTNET_EXE} ${MANAGED_TESTS})
  set_tests_properties(managed_scripting_tests PROPERTIES FIXTURES_REQUIRED managed_fx)

  add_test(NAME papyrus_hosttest COMMAND pexrun hosttest ${MANAGED_CFG} ${MANAGED_DLL})
  set_tests_properties(papyrus_hosttest PROPERTIES FIXTURES_REQUIRED managed_fx)

  add_test(NAME papyrus_managed_hosttest
    COMMAND pexrun managedhosttest ${MANAGED_CFG} ${MANAGED_DLL})
  set_tests_properties(papyrus_managed_hosttest PROPERTIES FIXTURES_REQUIRED managed_fx)
else()
  message(STATUS "dotnet not found: managed scripting tests disabled")
endif()

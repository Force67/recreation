# Managed (C#) scripting gates, wired only when a .NET SDK is on the configure
# PATH. A setup fixture builds the assembly and its test runner once; the unit
# tests cover the pure-managed modding framework, and the pexrun-driven tests
# drive the full C#<->engine dispatch path through the real bridge (they skip
# cleanly without a .NET runtime at test time).
find_program(DOTNET_EXE dotnet)
if(NOT DOTNET_EXE)
  message(STATUS "dotnet not found: managed scripting tests disabled")
  return()
endif()

set(MANAGED_OUT ${CMAKE_CURRENT_BINARY_DIR}/managed)
set(MANAGED_DLL ${MANAGED_OUT}/Recreation.Scripting.dll)
set(MANAGED_CFG ${MANAGED_OUT}/Recreation.Scripting.runtimeconfig.json)
set(MANAGED_TESTS ${MANAGED_OUT}/Recreation.Scripting.Tests.dll)

add_test(NAME managed_build
  COMMAND ${DOTNET_EXE} build ${CMAKE_CURRENT_SOURCE_DIR}/tests/Recreation.Scripting.Tests.csproj
          -c Release -o ${MANAGED_OUT} --nologo)
set_tests_properties(managed_build PROPERTIES FIXTURES_SETUP managed_fx)

# The default gamemodes build as their own assemblies; the SDK loads them from a
# gamemodes/ subdir beside itself (like the real runtime). The build lays every
# assembly flat, so stage the gamemode DLLs into gamemodes/ before the host tests
# run, else the Skyrim ruleset (AttributeRegeneration) never loads.
add_test(NAME managed_stage_gamemodes
  COMMAND ${CMAKE_COMMAND} -DDIR=${MANAGED_OUT}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/stage_gamemodes.cmake)
set_tests_properties(managed_stage_gamemodes PROPERTIES
  FIXTURES_SETUP managed_fx DEPENDS managed_build)

add_test(NAME managed_scripting_tests COMMAND ${DOTNET_EXE} ${MANAGED_TESTS})

add_test(NAME papyrus_hosttest COMMAND pexrun hosttest ${MANAGED_CFG} ${MANAGED_DLL})
add_test(NAME papyrus_managed_hosttest
  COMMAND pexrun managedhosttest ${MANAGED_CFG} ${MANAGED_DLL})

# Guards the two boundary tunings: the guest's same-thread Dispatch fast path
# and the NativeBackend arena marshalling. Runs the managed BridgeCheck battery
# through the real bridge from both the host thread and the guest thread; both
# must report identical, correct results (correctness only, no timing).
add_test(NAME papyrus_bridgetest COMMAND pexrun bridgetest ${MANAGED_CFG} ${MANAGED_DLL})

# Confirms the per-platform GC profile actually reaches the runtime: applies a
# heap hard limit and reads back the effective GC config through the managed
# GcReport probe (a bounded memory ceiling proves the property took effect).
add_test(NAME papyrus_gcprofiletest COMMAND pexrun gcprofiletest ${MANAGED_CFG} ${MANAGED_DLL})

set_tests_properties(
  managed_scripting_tests papyrus_hosttest papyrus_managed_hosttest
  papyrus_bridgetest papyrus_gcprofiletest
  PROPERTIES FIXTURES_REQUIRED managed_fx)

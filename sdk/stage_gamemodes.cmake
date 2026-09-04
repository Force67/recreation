# Stages the default gamemode assemblies into a gamemodes/ subdirectory of the
# managed build output, mirroring the real runtime layout (the SDK loads them from
# <sdk dir>/gamemodes/). The test build lays every assembly flat, so the host would
# otherwise find no gamemodes and skip AttributeRegeneration. Invoked as a ctest
# fixture step after the managed build; DIR is the flat managed output directory.
file(MAKE_DIRECTORY "${DIR}/gamemodes")
file(COPY "${DIR}/Recreation.Skyrim.dll"
          "${DIR}/Recreation.Fallout.dll"
          "${DIR}/Recreation.Starfield.dll"
          "${DIR}/Recreation.SkyrimCartRacing.dll"
          "${DIR}/Recreation.TourDeRecreation.dll"
     DESTINATION "${DIR}/gamemodes")

# Each assembly's menu manifest (and any key art beside it) lives with the
# gamemode sources; the C# build has no reason to copy them, so stage them here.
# The main menu reads them before the .NET runtime boots, which is the only way
# it can know a mode exists.
file(GLOB GAMEMODE_MANIFESTS
     "${CMAKE_CURRENT_LIST_DIR}/default_gamemodes/*/*.json"
     "${CMAKE_CURRENT_LIST_DIR}/default_gamemodes/*/*.png")
if(GAMEMODE_MANIFESTS)
  file(COPY ${GAMEMODE_MANIFESTS} DESTINATION "${DIR}/gamemodes")
endif()

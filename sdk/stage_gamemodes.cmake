# Stages the default gamemode assemblies into a gamemodes/ subdirectory of the
# managed build output, mirroring the real runtime layout (the SDK loads them from
# <sdk dir>/gamemodes/). The test build lays every assembly flat, so the host would
# otherwise find no gamemodes and skip AttributeRegeneration. Invoked as a ctest
# fixture step after the managed build; DIR is the flat managed output directory.
file(MAKE_DIRECTORY "${DIR}/gamemodes")
file(COPY "${DIR}/Recreation.Skyrim.dll"
          "${DIR}/Recreation.Fallout.dll"
          "${DIR}/Recreation.Starfield.dll"
     DESTINATION "${DIR}/gamemodes")

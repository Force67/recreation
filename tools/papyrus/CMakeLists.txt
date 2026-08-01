# Papyrus command-line tools. The VM, decompiler and bindings they drive live in
# components/script, and each of those has its own tests beside the code; what
# stays here are the tools plus the corpus suites pexrun itself runs.
set(PAPYRUS_TOOL_LIBS recreation::script recreation::bethesda rx::asset)

# pexdump: disassemble a .pex.        pexrun:  execute one, and the VM suites.
# vmaddump: dump a record's VMAD.     pex2cs:  EXPERIMENTAL Papyrus -> C#.
# nativescan: report which natives a data dir's scripts call.
# pexsoak / pexfuzz: corpus harnesses over a real Skyrim data directory.
foreach(tool pexdump pexrun vmaddump pex2cs nativescan pexsoak pexfuzz)
  add_executable(${tool} ${tool}.cc)
  target_link_libraries(${tool} PRIVATE ${PAPYRUS_TOOL_LIBS})
  recreation_set_warnings(${tool})
endforeach()

# Deterministic VM regression suite (no game data needed), so `ctest` is the
# one-command gate. The corpus checks (pexsoak/pexfuzz/nativescan) take the
# Skyrim data dir and run separately.
foreach(mode selftest skyrimtest guesttest conctest separationtest tracetest)
  add_test(NAME papyrus_${mode} COMMAND pexrun ${mode})
endforeach()

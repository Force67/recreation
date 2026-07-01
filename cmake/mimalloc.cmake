# mimalloc integration (RECREATION_MIMALLOC). Off by default; when on, the engine
# routes allocations through mimalloc for faster, lower-fragmentation heap use.
#
# Two layers of coverage (see the design note in the perf discussion):
#   Layer 1 - C++ operator new/delete. On POSIX these come from mimalloc's own
#             override, pulled in whole (below); on Windows, where a DLL cannot
#             override another module's new/delete, we compile
#             mimalloc-new-delete.h into each executable.
#   Layer 2 - the raw C malloc/free used by third-party libraries and the hosted
#             CLR's native allocations. POSIX: link mimalloc-static WHOLE so its
#             strong malloc/free symbols interpose libc's weak ones across the
#             whole process, including later dlopen'd libraries (the CLR). macOS:
#             the same static library registers a malloc zone at load. Windows has
#             no symbol interposition, so we ship mimalloc.dll + the prebuilt
#             mimalloc-redirect.dll beside the executable (the redirect patches
#             the CRT allocator at startup).
#
# The helper recreation_enable_mimalloc(<target>) is always defined and is a
# no-op when the option is off, so call sites stay unconditional.

if(RECREATION_MIMALLOC)
  # Prefer a system/toolchain mimalloc (e.g. from the nix dev shell); otherwise
  # build a pinned copy from source so every CI platform gets the same allocator.
  find_package(mimalloc 2.1 CONFIG QUIET)
  if(NOT mimalloc_FOUND)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)  # define malloc/free + new/delete
    # Keep both the static lib (POSIX interpose) and the shared DLL (Windows).
    FetchContent_Declare(mimalloc
      GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
      GIT_TAG v2.1.7
      GIT_SHALLOW ON)
    FetchContent_MakeAvailable(mimalloc)
  endif()
  message(STATUS "mimalloc enabled")
endif()

# Applies the enabled mimalloc coverage to one executable target.
function(recreation_enable_mimalloc target)
  if(NOT RECREATION_MIMALLOC)
    return()
  endif()
  target_compile_definitions(${target} PRIVATE RECREATION_MIMALLOC=1)
  if(WIN32)
    # Layer 1: operator new/delete override compiled into this binary.
    target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/mimalloc_override.cc)
    # Layer 2: dynamic override via the DLL, whose redirect patches the CRT.
    target_link_libraries(${target} PRIVATE mimalloc)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:mimalloc> $<TARGET_FILE_DIR:${target}>
      COMMENT "mimalloc: staging mimalloc.dll beside ${target}"
      VERBATIM)
    # mimalloc-redirect.dll is a prebuilt binary shipped in the mimalloc tree; it
    # must sit next to mimalloc.dll for the CRT patch to take effect.
    if(DEFINED mimalloc_SOURCE_DIR AND
       EXISTS "${mimalloc_SOURCE_DIR}/bin/mimalloc-redirect.dll")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${mimalloc_SOURCE_DIR}/bin/mimalloc-redirect.dll" $<TARGET_FILE_DIR:${target}>
        VERBATIM)
    endif()
  else()
    # Layers 1+2: link the static override whole, so mimalloc's malloc/free and
    # operator new/delete replace libc's / libstdc++'s across the process.
    target_link_libraries(${target} PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,mimalloc-static>)
  endif()
endfunction()

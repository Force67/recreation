# NVIDIA NRD real time denoiser, downloaded via tools/get_nrd.sh and built from
# source. Consumed through the direct Vulkan path (no NRI): the engine reads the
# embedded SPIR-V pipelines from nrd::GetInstanceDesc and records the dispatches
# itself (see engine/render/denoiser_nrd.cc).
#
# NRD's CMake pulls MathLib and ShaderMake through FetchContent. The nix build
# configures with FETCHCONTENT_FULLY_DISCONNECTED, so both are vendored next to
# NRD and redirected here. ShaderMake would otherwise also download DXC from
# GitHub; SHADERMAKE_FIND_DXC is forced off so it finds the dev shell dxc (which
# already produces the engine's own SPIR-V) on PATH instead.

set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE ${CMAKE_CURRENT_SOURCE_DIR}/NRD-ShaderMake
    CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_MATHLIB ${CMAKE_CURRENT_SOURCE_DIR}/NRD-MathLib
    CACHE PATH "" FORCE)

set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS ON CACHE BOOL "" FORCE)
# The engine only consumes the SPIR-V pipelines (direct Vulkan path); NRD
# otherwise defaults the DXIL and DXBC containers on for Windows, whose
# ShaderMake commands need a DXC/FXC the build doesn't wire up (an empty
# --compiler path makes ShaderMake misread the next flag as the compiler).
set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_NRI OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_DXC OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_SLANG OFF CACHE BOOL "" FORCE)
# ShaderMake finds the SPIR-V DXC from the Vulkan SDK or PATH; on Windows it only
# probes %VULKAN_SDK%, so seed the cache with the dxc the workflow already put on
# PATH (the same compiler the engine uses for its own shaders) before NRD's
# subdirectory runs its own find_program.
if(WIN32)
  find_program(SHADERMAKE_DXC_VK_PATH NAMES dxc)
endif()
# Keep the generated shader headers out of the source tree.
set(NRD_SHADERS_PATH ${CMAKE_BINARY_DIR}/nrd_shaders CACHE STRING "" FORCE)

add_subdirectory(NRD nrd EXCLUDE_FROM_ALL)
add_library(recreation::nrd ALIAS NRD)

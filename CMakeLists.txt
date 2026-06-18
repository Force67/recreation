cmake_minimum_required(VERSION 3.24)
project(recreation VERSION 0.1.0 LANGUAGES CXX)

# equilibrium (vendored by zetanet) needs C++23, the engine uses it as its
# container and string layer.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RECREATION_BUILD_TOOLS "Build offline tools" ON)
option(RECREATION_ENABLE_VALIDATION "Enable vulkan validation layers in debug" ON)
option(RECREATION_FETCH_SDL3 "Download SDL3 when not found on the system" OFF)
option(RECREATION_FSR3 "Enable the AMD FSR 3.1 upscaler backend" ON)
option(RECREATION_JOLT "Enable Jolt physics" ON)

if(RECREATION_FSR3 AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/FidelityFX-SDK/sdk/include/FidelityFX/host/ffx_fsr3upscaler.h)
  message(STATUS "FidelityFX-SDK not found, fsr3 disabled (run tools/get_fidelityfx.sh)")
  set(RECREATION_FSR3 OFF)
endif()

if(RECREATION_JOLT AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics/Jolt/Jolt.cmake)
  message(STATUS "JoltPhysics not found, physics disabled (run tools/get_jolt.sh)")
  set(RECREATION_JOLT OFF)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(warnings)

include(FetchContent)
find_package(ZLIB QUIET)

# Vulkan headers and the volk loader are pinned and fetched, no SDK needed.
# volk dlopens the system vulkan loader at runtime, missing drivers degrade
# the renderer to a stub instead of failing the build.
FetchContent_Declare(VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.304
  GIT_SHALLOW ON)
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.4.304
  GIT_SHALLOW ON)
FetchContent_Declare(VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.1.0
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(VulkanHeaders volk VulkanMemoryAllocator)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# Shaders are hlsl, compiled to spirv at build time with dxc and embedded.
find_program(RECREATION_DXC dxc)
if(NOT RECREATION_DXC)
  message(FATAL_ERROR "dxc (DirectXShaderCompiler) is required to compile hlsl to spirv")
endif()
include(shaders)

find_package(SDL3 QUIET)
if(NOT SDL3_FOUND AND RECREATION_FETCH_SDL3)
  set(SDL_TEST_LIBRARY OFF)
  # Static keeps the artifacts self contained, no SDL3.dll shipping.
  set(SDL_SHARED OFF)
  set(SDL_STATIC ON)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.16
    GIT_SHALLOW ON)
  FetchContent_MakeAvailable(SDL3)
  set(SDL3_FOUND TRUE)
endif()
if(SDL3_FOUND)
  message(STATUS "sdl3 found, native windowing enabled")
else()
  message(STATUS "sdl3 not found, runtime is headless")
endif()

# Zetanet (devel branch checkout) supplies the UDP transport and reliability
# layer. Built in the equilibrium configuration so base:: containers, strings
# and atomics are shared between the engine and the network stack.
set(RECREATION_ZETANET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zetanet" CACHE PATH
  "Path to a zetanet checkout (devel branch)")
if(NOT EXISTS "${RECREATION_ZETANET_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "zetanet not found at ${RECREATION_ZETANET_DIR}, set RECREATION_ZETANET_DIR")
endif()
set(ZNET_USE_STL OFF CACHE BOOL "" FORCE)
set(ZNET_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${RECREATION_ZETANET_DIR} zetanet EXCLUDE_FROM_ALL)

# Nanobuf carries every message that crosses the wire. Schemas live in nbuf/,
# generated bindings in nbuf/gen/ (checked in, so builds don't need rust), and
# nbuf/runtime/ vendors the header-only C++ runtime.
add_library(recreation_nanobuf INTERFACE)
add_library(recreation::nanobuf ALIAS recreation_nanobuf)
target_include_directories(recreation_nanobuf INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/gen
  ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/runtime)

find_program(RECREATION_NANOC nanoc
  HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../nanobuf/target/release)
if(RECREATION_NANOC)
  add_custom_target(nanobuf_regen
    COMMAND ${RECREATION_NANOC} build ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/protocol.nb
            --lang cpp --out ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/gen
    COMMENT "regenerating nanobuf bindings from nbuf/*.nb")
endif()

add_subdirectory(third_party)

set(RECREATION_MODULES
  core
  ecs
  asset
  render
  physics
  bethesda
  world
  net
  script
)

foreach(module ${RECREATION_MODULES})
  add_subdirectory(engine/${module})
endforeach()

add_subdirectory(runtime)

if(RECREATION_BUILD_TOOLS)
  add_subdirectory(tools/esminfo)
endif()

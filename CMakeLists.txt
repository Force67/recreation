cmake_minimum_required(VERSION 3.24)
project(recreation VERSION 0.1.0 LANGUAGES CXX C)

# equilibrium (vendored by zetanet) needs C++23, the engine uses it as its
# container and string layer.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RECREATION_BUILD_TOOLS "Build offline tools" ON)
option(RECREATION_ENABLE_VALIDATION "Enable vulkan validation layers in debug" ON)
option(RECREATION_FETCH_SDL3 "Download SDL3 when not found on the system" OFF)
option(RECREATION_FSR3 "Enable the AMD FSR 3.1 upscaler backend" ON)
option(RECREATION_DLSS "Enable the NVIDIA DLSS upscaler backend" ON)
option(RECREATION_NRD "Enable the NVIDIA NRD ray tracing denoiser" ON)
option(RECREATION_JOLT "Enable Jolt physics" ON)
option(RECREATION_NET "Enable networking (zetanet transport + replication)" ON)
# FFmpeg (libav*) decodes the games' compressed audio: xWMA (Skyrim/Fallout 4
# music + ambience), the WMA inside FUZ voice files, and the Wwise codecs in
# Starfield .wem. Off by default so the CI matrix carries no heavyweight system
# dependency; on, the audio module dynamically links libav* (shipped as shared
# libraries beside the executable). Without it those formats are skipped cleanly.
option(RECREATION_AUDIO_FFMPEG "Decode xWMA/Wwise/FUZ audio via FFmpeg (libav*)" OFF)
option(RECREATION_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Android is a first-class target, but several desktop backends do not apply:
# DLSS/FSR3/NRD ship x86_64 host libraries (and Adreno has no HW ray tracing or
# mesh shaders, so the raster path is the real path), SDL3 is replaced by a
# native ANativeWindow backend, and zetanet's crypto backend (OpenSSL/mbedTLS)
# is not in the NDK sysroot. Force them off so an Android configure is clean.
if(ANDROID)
  set(RECREATION_DLSS OFF CACHE BOOL "" FORCE)
  set(RECREATION_FSR3 OFF CACHE BOOL "" FORCE)
  set(RECREATION_NRD OFF CACHE BOOL "" FORCE)
  set(RECREATION_NET OFF CACHE BOOL "" FORCE)
  set(RECREATION_FETCH_SDL3 OFF CACHE BOOL "" FORCE)
  set(RECREATION_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
endif()

# macOS runs Vulkan through MoltenVK (Vulkan-on-Metal). The three GPU-vendor
# backends do not apply: DLSS ships an x86_64 NVIDIA-only host library, and the
# FSR3/NRD shader-permutation toolchains have no validated MoltenVK path (their
# vk backends target native AMD/NVIDIA Vulkan). The raster path is the real path
# here, so force them off for a clean Apple configure; windowing (SDL3/Metal),
# physics (Jolt), networking and the tools all stay on. The portability bits the
# loader needs are handled in the renderer (see engine/render/vulkan).
if(APPLE)
  set(RECREATION_DLSS OFF CACHE BOOL "" FORCE)
  set(RECREATION_FSR3 OFF CACHE BOOL "" FORCE)
  set(RECREATION_NRD OFF CACHE BOOL "" FORCE)
endif()

# Sanitizer build for the regression suite: the papyrus vm (parser, interpreter,
# native dispatch, threaded guest) is validated memory-safe, leak-free and
# UB-free over the whole shipped corpus under these. Applied globally so the
# instrumentation is consistent across the engine and the tools.
if(RECREATION_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(RECREATION_FSR3 AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/FidelityFX-SDK/sdk/include/FidelityFX/host/ffx_fsr3upscaler.h)
  message(STATUS "FidelityFX-SDK not found, fsr3 disabled (run tools/get_fidelityfx.sh)")
  set(RECREATION_FSR3 OFF)
endif()

if(RECREATION_DLSS AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/DLSS/lib/Linux_x86_64/libnvsdk_ngx.a)
  message(STATUS "DLSS SDK not found, dlss disabled (run tools/get_dlss.sh)")
  set(RECREATION_DLSS OFF)
endif()

if(RECREATION_NRD AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NRD/Include/NRD.h)
  message(STATUS "NRD not found, denoiser disabled (run tools/get_nrd.sh)")
  set(RECREATION_NRD OFF)
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
if(ANDROID)
  # So volk's loader compiles the android-surface entry point and consumers see
  # vkCreateAndroidSurfaceKHR / VkAndroidSurfaceCreateInfoKHR.
  target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR=1)
endif()

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

# equilibrium::base is vendored directly here (third_party/equilibrium, a
# submodule) and is its single source of truth, so base:: containers, strings
# and atomics are shared between the engine and the network stack.
# EQ_USE_STD_ATOMIC matches the std::memory_order_* zetanet hands to base::Atomic.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium/CMakeLists.txt")
  message(FATAL_ERROR
    "equilibrium submodule missing; run: git submodule update --init third_party/equilibrium")
endif()
set(EQ_USE_STD_ATOMIC ON CACHE BOOL "" FORCE)
set(EQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# equilibrium's Windows base formats with fmtlib (which it does not vendor) and
# errors out unless EQ_FMTLIB_DIR points at an fmt checkout. Reuse the one
# zetanet already vendors; other platforms don't need it.
if(WIN32 AND NOT EQ_FMTLIB_DIR)
  set(EQ_FMTLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zetanet/vendor/fmtlib" CACHE PATH "" FORCE)
endif()
add_subdirectory(third_party/equilibrium equilibrium EXCLUDE_FROM_ALL)

# Zetanet (devel branch checkout) supplies the UDP transport and reliability
# layer, built in equilibrium mode (ZNET_USE_STL=OFF). It hard-codes
# add_subdirectory(vendor/equilibrium); rather than patch upstream zetanet, we
# locally override add_subdirectory so that one call reuses the equilibrium::base
# vendored above instead of compiling a second, conflicting copy.
set(RECREATION_ZETANET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zetanet" CACHE PATH
  "Path to a zetanet checkout (devel branch)")
if(RECREATION_NET)
  if(NOT EXISTS "${RECREATION_ZETANET_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "zetanet not found at ${RECREATION_ZETANET_DIR}, set RECREATION_ZETANET_DIR")
  endif()
  set(ZNET_USE_STL OFF CACHE BOOL "" FORCE)
  set(ZNET_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  function(add_subdirectory _dir)
    get_filename_component(_leaf "${_dir}" NAME)
    if(_leaf STREQUAL "equilibrium" AND TARGET equilibrium::base)
      return()  # zetanet's vendored copy: reuse the one recreation vendors
    endif()
    _add_subdirectory("${_dir}" ${ARGN})
  endfunction()
  add_subdirectory(${RECREATION_ZETANET_DIR} zetanet EXCLUDE_FROM_ALL)
endif()

# Nanobuf carries every message that crosses the wire. Recreation's schemas
# live in nbuf/; the wire bindings are regenerated from them at configure time
# by nanoc (the nanobuf schema compiler), so the protocol always matches both
# the schema and the released compiler in use.
#
# nanoc and its matching C++ runtime header are taken, in order of preference:
#   1. third_party/nanobuf/ -- the released toolchain fetched by
#      tools/get_nanobuf.sh. This is how CI and release builds get it: a single
#      prebuilt binary, no Rust and no nanobuf checkout.
#   2. a nanoc on PATH -- the nix dev shell builds one from the local checkout.
#   3. RECREATION_NANOBUF_DIR -- a sibling nanobuf source / cargo tree.
# With none of these, the bindings checked into nbuf/gen and the runtime in
# nbuf/runtime are used unchanged, so a bare checkout still builds offline.
set(RECREATION_NANOBUF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../nanobuf" CACHE PATH
  "Path to a nanobuf checkout (supplies nanoc and the C++ runtime header)")
set(_nanobuf_fetched "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanobuf")
set(_nanobuf_schema "${CMAKE_CURRENT_SOURCE_DIR}/nbuf/protocol.nb")

find_program(RECREATION_NANOC nanoc
  HINTS ${_nanobuf_fetched}/bin
        ${RECREATION_NANOBUF_DIR}/target/release
        ${RECREATION_NANOBUF_DIR}/target/debug)

# The runtime header must pair with whichever nanoc was found.
if(EXISTS ${_nanobuf_fetched}/include/nanobuf.h)
  set(_nanobuf_runtime_dir ${_nanobuf_fetched}/include)
elseif(EXISTS ${RECREATION_NANOBUF_DIR}/runtimes/cpp/nanobuf.h)
  set(_nanobuf_runtime_dir ${RECREATION_NANOBUF_DIR}/runtimes/cpp)
else()
  set(_nanobuf_runtime_dir ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/runtime)
endif()

add_library(recreation_nanobuf INTERFACE)
add_library(recreation::nanobuf ALIAS recreation_nanobuf)

option(RECREATION_NANOBUF_REGEN
  "Regenerate the wire protocol from nbuf/*.nb with nanoc" ON)

if(RECREATION_NANOC AND RECREATION_NANOBUF_REGEN)
  set(_nanobuf_gen ${CMAKE_BINARY_DIR}/nbuf-gen)
  file(MAKE_DIRECTORY ${_nanobuf_gen})
  execute_process(
    COMMAND ${RECREATION_NANOC} build ${_nanobuf_schema} --lang cpp --out ${_nanobuf_gen}
    RESULT_VARIABLE _nanobuf_rc
    OUTPUT_VARIABLE _nanobuf_log
    ERROR_VARIABLE _nanobuf_log)
  if(NOT _nanobuf_rc EQUAL 0)
    message(FATAL_ERROR "nanoc failed to regenerate the protocol:\n${_nanobuf_log}")
  endif()
  target_include_directories(recreation_nanobuf INTERFACE
    ${_nanobuf_gen} ${_nanobuf_runtime_dir})
  message(STATUS "nanobuf: protocol regenerated with ${RECREATION_NANOC}")

  # Refresh the checked-in copy (the offline fallback) so it can be committed.
  add_custom_target(nanobuf_regen
    COMMAND ${RECREATION_NANOC} build ${_nanobuf_schema}
            --lang cpp --out ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/gen
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_nanobuf_runtime_dir}/nanobuf.h
            ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/runtime/nanobuf.h
    COMMENT "refreshing checked-in nbuf/gen + nbuf/runtime"
    VERBATIM)
else()
  target_include_directories(recreation_nanobuf INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/nbuf/runtime)
  if(NOT RECREATION_NANOC)
    message(STATUS "nanobuf: nanoc not found, using checked-in bindings in nbuf/gen "
                   "(run tools/get_nanobuf.sh to fetch the released compiler)")
  endif()
endif()

add_subdirectory(third_party)

# libultragui: GPU UI middleware (sibling checkout) for the HUD and menus. Built
# in draw-data mode with a custom SDL-fed platform and a headless RHI, so it
# pulls in neither GLFW nor a second Vulkan path; the engine renders its draw
# list with its own pipelines. Only freetype + harfbuzz + yoga are needed.
# Skipped (HUD compiles out) when the checkout or SDL3 is missing.
set(RECREATION_LIBULTRAGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libultragui" CACHE PATH
  "Path to a libultragui checkout")
set(RECREATION_HAS_UGUI OFF)
if(SDL3_FOUND AND EXISTS "${RECREATION_LIBULTRAGUI_DIR}/CMakeLists.txt")
  set(ULTRAGUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_BACKEND_VULKAN OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_LUA OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_AUDIO OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_LOTTIE OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_VIDEO OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_PLATFORM_GLFW OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_BUILD_SHADERS OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_PLATFORM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ugui_platform.cc"
    CACHE FILEPATH "" FORCE)
  set(ULTRAGUI_RHI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ugui_rhi.cc"
    CACHE FILEPATH "" FORCE)
  # Swap ultragui's scripting runtime for recreation's: ugui handlers run in the
  # hosted .NET (C#) world instead of Lua. The backend stays library-agnostic and
  # reaches the managed host through runtime/ugui_csharp_host.h (see that file).
  set(ULTRAGUI_SCRIPT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ugui_script_csharp.cc"
    CACHE FILEPATH "" FORCE)
  add_subdirectory(${RECREATION_LIBULTRAGUI_DIR} libultragui EXCLUDE_FROM_ALL)
  set(RECREATION_HAS_UGUI ON)
  message(STATUS "libultragui found, hud enabled")
else()
  message(STATUS "libultragui not found, hud disabled")
endif()

set(RECREATION_MODULES
  core
  ecs
  asset
  render
  physics
  anim
  bethesda
  audio
  world
  quest
  weather
  dialogue
  script
  modstream
  rpc
)
if(RECREATION_NET)
  list(APPEND RECREATION_MODULES net)
endif()

foreach(module ${RECREATION_MODULES})
  add_subdirectory(engine/${module})
endforeach()

add_subdirectory(runtime)

if(RECREATION_BUILD_TOOLS)
  enable_testing()  # papyrus vm regression suite, run with `ctest`
  add_subdirectory(tools/esminfo)
  add_subdirectory(tools/modtool)
  add_subdirectory(tools/papyrus)
endif()

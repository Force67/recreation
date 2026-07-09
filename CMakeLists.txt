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
# Route the engine's allocations (and, on platforms with symbol interposition,
# raw C malloc from third-party libs and the hosted CLR's native calls) through
# mimalloc for faster, lower-fragmentation heap use. Off by default: opt-in per
# build, measured before it becomes a default. Incompatible with the sanitizer
# build (ASan owns the allocator), so it is forced off under RECREATION_SANITIZE.
option(RECREATION_MIMALLOC "Route allocations through mimalloc" OFF)

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

# The FidelityFX/DLSS/NRD/Jolt SDKs now live under the rx engine checkout, which
# runs its own existence checks against its own third_party tree; the
# RECREATION_FSR3/DLSS/NRD/JOLT options are forwarded to rx below.

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

# Optional mimalloc allocator (defines recreation_enable_mimalloc(); no-op when
# the option is off). Included here so the helper exists before the runtime
# executables are declared.
if(RECREATION_MIMALLOC AND RECREATION_SANITIZE)
  message(STATUS "mimalloc disabled: RECREATION_SANITIZE owns the allocator")
  set(RECREATION_MIMALLOC OFF CACHE BOOL "" FORCE)
endif()
include(mimalloc)

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

# The rx engine (sibling checkout) supplies the generic engine modules
# (core/ecs/asset/render/physics/anim/audio/rpc), the imgui/cgltf/stb vendored
# libs and the FSR3/DLSS/NRD/Jolt SDKs. recreation stays the provider for
# volk/VMA/equilibrium (rx guards those on `if(NOT TARGET ...)` so they skip),
# and applies its own mimalloc helper to its executables (RX_MIMALLOC off).
set(RECREATION_RX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rx" CACHE PATH "Path to an rx engine checkout")
if(NOT EXISTS "${RECREATION_RX_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "rx not found at ${RECREATION_RX_DIR}, set RECREATION_RX_DIR")
endif()
set(RX_BUILD_RUNTIME OFF CACHE BOOL "" FORCE)
set(RX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RX_FSR3 ${RECREATION_FSR3} CACHE BOOL "" FORCE)
set(RX_DLSS ${RECREATION_DLSS} CACHE BOOL "" FORCE)
set(RX_NRD ${RECREATION_NRD} CACHE BOOL "" FORCE)
set(RX_JOLT ${RECREATION_JOLT} CACHE BOOL "" FORCE)
set(RX_AUDIO_FFMPEG ${RECREATION_AUDIO_FFMPEG} CACHE BOOL "" FORCE)
set(RX_SANITIZE ${RECREATION_SANITIZE} CACHE BOOL "" FORCE)
set(RX_MIMALLOC OFF CACHE BOOL "" FORCE)
# rx ships cmake/{warnings,shaders,mimalloc,...}.cmake with the same file names
# as recreation's but different helper names (rx_add_module vs
# recreation_add_module, etc). recreation's cmake dir is already first on
# CMAKE_MODULE_PATH, so rx's own include(warnings)/include(shaders) would resolve
# to recreation's copies and never define the rx_* helpers. Clear the path for
# rx's configure (it appends its own cmake dir) and restore it afterwards.
set(_recreation_saved_module_path "${CMAKE_MODULE_PATH}")
set(CMAKE_MODULE_PATH "")
add_subdirectory(${RECREATION_RX_DIR} rx)
set(CMAKE_MODULE_PATH "${_recreation_saved_module_path}")

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

# The generic engine modules (core/ecs/asset/render/physics/anim/rpc) and the
# kinema animation runtime now come from rx; recreation keeps only its Bethesda
# game modules.
set(RECREATION_MODULES
  bethesda
  audio
  world
  quest
  weather
  dialogue
  script
  modstream
)
if(RECREATION_NET)
  list(APPEND RECREATION_MODULES net)
endif()

if(RECREATION_BUILD_TOOLS)
  enable_testing()  # papyrus vm regression suite, run with `ctest`
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

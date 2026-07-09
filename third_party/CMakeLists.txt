# Vendored third party code. Kept in tree because the nix build configures
# with FETCHCONTENT_FULLY_DISCONNECTED, so anything fetched needs a flake
# input; small single-header or few-file libraries are simpler vendored.
#
# imgui, cgltf, the FSR3/DLSS/NRD upscalers/denoiser and Jolt now come from the
# rx engine checkout (added as a subdirectory in the top-level CMakeLists). Only
# stb stays vendored here: the ultragui HUD backend (runtime) links it.

add_library(recreation_stb INTERFACE)
add_library(recreation::stb ALIAS recreation_stb)
target_include_directories(recreation_stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

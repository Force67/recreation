# Vendored third party code. Kept in tree because the nix build configures
# with FETCHCONTENT_FULLY_DISCONNECTED, so anything fetched needs a flake
# input; small single-header or few-file libraries are simpler vendored.

# Dear ImGui (docking branch) with the SDL3 platform and Vulkan renderer
# backends. The Vulkan backend resolves entry points through volk, matching
# the engine's loader setup. Only built when a window backend exists.
if(SDL3_FOUND)
  add_library(recreation_imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_vulkan.cpp
  )
  add_library(recreation::imgui ALIAS recreation_imgui)
  target_include_directories(recreation_imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends)
  target_compile_definitions(recreation_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
  target_link_libraries(recreation_imgui PUBLIC SDL3::SDL3 volk)
endif()

add_library(recreation_cgltf INTERFACE)
add_library(recreation::cgltf ALIAS recreation_cgltf)
target_include_directories(recreation_cgltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)

add_library(recreation_stb INTERFACE)
add_library(recreation::stb ALIAS recreation_stb)
target_include_directories(recreation_stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

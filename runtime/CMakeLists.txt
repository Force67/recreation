# Engine sources shared by every front end (desktop client, dedicated server,
# Android client). The debug overlay (imgui) and HUD (ugui) compile to stubs
# when those middlewares are absent, so the same translation units serve all.
set(RECREATION_RUNTIME_SOURCES
  engine.cc
  fly_camera.cc
  debug_ui.cc
  game_ui.cc
)

set(RECREATION_RUNTIME_MODULES
  recreation::core
  recreation::ecs
  recreation::asset
  recreation::render
  recreation::physics
  recreation::anim
  recreation::bethesda
  recreation::world
  recreation::quest
  recreation::script
)

if(ANDROID)
  # Android client: a shared library a NativeActivity loads. The NDK's native
  # glue translation unit provides ANativeActivity_onCreate and the looper
  # scaffolding android_main.cc builds on.
  if(NOT ANDROID_NDK)
    set(ANDROID_NDK ${CMAKE_ANDROID_NDK})
  endif()
  add_library(android_native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(android_native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)
  target_link_libraries(android_native_app_glue PUBLIC android log)

  add_library(recreation SHARED android_main.cc ${RECREATION_RUNTIME_SOURCES})
  target_link_libraries(recreation PRIVATE
    ${RECREATION_RUNTIME_MODULES}
    android_native_app_glue
    android
    log)
  recreation_set_warnings(recreation)
  # ANativeActivity_onCreate is only referenced by the Java runtime, so force
  # the linker to keep it (and pull in the glue object that defines it).
  target_link_options(recreation PRIVATE -u ANativeActivity_onCreate)
  return()
endif()

add_executable(recreation main.cc ${RECREATION_RUNTIME_SOURCES})
target_link_libraries(recreation PRIVATE ${RECREATION_RUNTIME_MODULES})
recreation_set_warnings(recreation)

if(RECREATION_NET)
  target_link_libraries(recreation PRIVATE recreation::net)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_NET=1)
endif()

if(TARGET recreation_imgui)
  target_link_libraries(recreation PRIVATE recreation::imgui)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_IMGUI=1)
endif()

# libultragui HUD: only the windowed client builds it; the dedicated server
# compiles game_ui.cc to a stub. The draw list is rendered by gui_backend.cc
# with engine-embedded shaders.
if(RECREATION_HAS_UGUI)
  target_sources(recreation PRIVATE gui_backend.cc)
  target_link_libraries(recreation PRIVATE ultragui)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_UGUI=1)
  recreation_embed_shaders(recreation
    shaders/ugui_quad.vs.hlsl
    shaders/ugui_quad.ps.hlsl
    shaders/ugui_text.vs.hlsl
    shaders/ugui_text.ps.hlsl
  )
endif()

# Dedicated server: same engine, headless config baked in. Still links the
# renderer module since the engine owns one; it never initializes vulkan.
add_executable(recreation-server server_main.cc ${RECREATION_RUNTIME_SOURCES})
target_link_libraries(recreation-server PRIVATE ${RECREATION_RUNTIME_MODULES})
recreation_set_warnings(recreation-server)

if(RECREATION_NET)
  target_link_libraries(recreation-server PRIVATE recreation::net)
  target_compile_definitions(recreation-server PRIVATE RECREATION_HAS_NET=1)
endif()

# Engine sources shared by every front end (desktop client, dedicated server,
# Android client). The debug overlay (imgui) and HUD (ugui) compile to stubs
# when those middlewares are absent, so the same translation units serve all.
set(RECREATION_RUNTIME_SOURCES
  engine.cc
  frame_loop.cc
  networking.cc
  managed_rpc.cc
  content_load.cc
  managed_scripting.cc
  main_menu.cc
  first_run.cc
  camera_input.cc
  controls_settings.cc
  content_domain.cc
  actor_system.cc
  demo_scenes.cc
  interaction_system.cc
  npc_director.cc
  platform_hud.cc
  quest_director.cc
  fly_camera.cc
  showcase_camera.cc
  trailer.cc
  editor.cc
  editor_catalog.cc
  editor_io.cc
  thumbnailer.cc
  debug_ui.cc
  game_ui.cc
)

set(RECREATION_RUNTIME_MODULES
  recreation::core
  recreation::ecs
  recreation::asset
  recreation::audio
  recreation::render
  recreation::physics
  recreation::anim
  recreation::bethesda
  recreation::world
  recreation::quest
  recreation::weather
  recreation::dialogue
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
target_compile_definitions(recreation PRIVATE RECREATION_VERSION="${PROJECT_VERSION}")
recreation_set_warnings(recreation)

if(RECREATION_NET)
  target_link_libraries(recreation PRIVATE recreation::net)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_NET=1)
endif()

if(TARGET recreation_imgui)
  target_link_libraries(recreation PRIVATE recreation::imgui)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_IMGUI=1)
  # Default location of the editable .ini render presets (engine/render/presets).
  # The debug ui lists and loads them; override with the env var REC_PRESETS_DIR.
  # Absolute so the dev build finds the source files.
  target_compile_definitions(recreation PRIVATE
    RECREATION_PRESETS_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/engine/render/presets")
endif()

# libultragui HUD: only the windowed client builds it; the dedicated server
# compiles game_ui.cc to a stub. The draw list is rendered by gui_backend.cc
# with engine-embedded shaders.
if(RECREATION_HAS_UGUI)
  target_sources(recreation PRIVATE gui_backend.cc)
  target_link_libraries(recreation PRIVATE ultragui recreation::stb)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_UGUI=1)
  # Default location of the editable .ugui screen fragments (runtime/ui). The
  # client reads them at startup and hot-reloads them; override with the env var
  # RECREATION_UI_DIR. Absolute so the dev build finds the source files.
  target_compile_definitions(recreation PRIVATE
    RECREATION_UI_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/ui")
  recreation_embed_shaders(recreation
    shaders/ugui_quad.vs.hlsl
    shaders/ugui_quad.ps.hlsl
    shaders/ugui_text.vs.hlsl
    shaders/ugui_text.ps.hlsl
    shaders/ugui_frost.vs.hlsl
    shaders/ugui_frost.ps.hlsl
    shaders/thumb.vs.hlsl
    shaders/thumb.ps.hlsl
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

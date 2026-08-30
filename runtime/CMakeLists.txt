# Engine sources shared by every front end (desktop client, dedicated server,
# Android client). The debug overlay (imgui) and HUD (ugui) compile to stubs
# when those middlewares are absent, so the same translation units serve all.
set(RECREATION_RUNTIME_SOURCES
  app/engine.cc
  app/frame_loop.cc
  app/networking.cc
  app/managed_rpc.cc
  app/content_load.cc
  app/managed_scripting.cc
  app/main_menu.cc
  app/first_run.cc
  app/camera_input.cc
  app/controls_settings.cc
  app/content_domain.cc
  input/game_input.cc
  camera/fly_camera.cc
  camera/showcase_camera.cc
  actor/actor_system.cc
  actor/player_controller.cc
  actor/nav_bubble.cc
  actor/ai_package_director.cc
  actor/npc_director.cc
  character/face.cc
  character/chargen.cc
  character/fp_equipment.cc
  vehicle/carriage.cc
  vehicle/cart_visuals.cc
  narrative/helgen_intro.cc
  narrative/cutscene_director.cc
  narrative/quest_director.cc
  interaction/interaction_system.cc
  interaction/item_bridge.cc
  demo/demo_scenes.cc
  demo/trailer.cc
  editor/editor.cc
  editor/editor_catalog.cc
  editor/editor_io.cc
  ui/platform_hud.cc
  ui/thumbnailer.cc
  ui/debug_ui.cc
  ui/game_ui.cc
)

set(RECREATION_RUNTIME_MODULES
  rx::core
  rx::ecs
  rx::asset
  rx::app
  recreation::audio
  rx::render
  rx::physics
  rx::anim
  rx::nav
  rx::nav_viz
  rx::scene
  rx::character
  rx::inventory
  rx::inventory_world
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

  add_library(recreation SHARED app/android_main.cc ${RECREATION_RUNTIME_SOURCES})
  target_link_libraries(recreation PRIVATE
    ${RECREATION_RUNTIME_MODULES}
    android_native_app_glue
    android
    log)
  recreation_set_warnings(recreation)
  recreation_enable_mimalloc(recreation)
  # ANativeActivity_onCreate is only referenced by the Java runtime, so force
  # the linker to keep it (and pull in the glue object that defines it).
  target_link_options(recreation PRIVATE -u ANativeActivity_onCreate)
  return()
endif()

add_executable(recreation app/main.cc ${RECREATION_RUNTIME_SOURCES})
target_link_libraries(recreation PRIVATE ${RECREATION_RUNTIME_MODULES})
target_compile_definitions(recreation PRIVATE RECREATION_VERSION="${PROJECT_VERSION}")
recreation_set_warnings(recreation)
recreation_enable_mimalloc(recreation)

if(RECREATION_NET)
  target_link_libraries(recreation PRIVATE recreation::gamenet rx::net_viz)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_NET=1)
endif()

if(TARGET rx_imgui)
  target_link_libraries(recreation PRIVATE rx::imgui)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_IMGUI=1)
  # Default location of the editable .ini render presets (they live in the rx
  # render module now). The debug ui lists and loads them; override with the env
  # var RX_PRESETS_DIR. Absolute so the dev build finds the source files.
  target_compile_definitions(recreation PRIVATE
    RECREATION_PRESETS_DIR_DEFAULT="${RECREATION_RX_DIR}/engine/render/presets")
endif()

# libultragui HUD: only the windowed client builds it; the dedicated server
# compiles ui/game_ui.cc to a stub. The draw list is rendered by gui_backend.cc
# with engine-embedded shaders.
if(RECREATION_HAS_UGUI)
  target_sources(recreation PRIVATE ui/gui_backend.cc ui/shader_pack.cc ui/vanilla_ui.cc
                                     ui/vanilla_list.cc
                                     ui/vanilla_runtime.cc)
  # The vanilla screens' own ActionScript runs on the swf module's interpreter.
  target_link_libraries(recreation PRIVATE ultragui recreation::stb recreation::swf)
  target_compile_definitions(recreation PRIVATE RECREATION_HAS_UGUI=1)
  # The recreation-owned HUD/thumbnail shaders ship in this archive; the engine
  # mounts it under shaders:// at startup (override the path with the env var
  # RECREATION_SHADER_PACK). Absolute so the dev build finds the build output.
  target_compile_definitions(recreation PRIVATE
    RECREATION_SHADER_PACK_DEFAULT="${CMAKE_BINARY_DIR}/shaders.rxp")
  # Default location of the editable .ugui screen fragments (runtime/ui). The
  # client reads them at startup and hot-reloads them; override with the env var
  # RECREATION_UI_DIR. Absolute so the dev build finds the source files.
  target_compile_definitions(recreation PRIVATE
    RECREATION_UI_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/ui/screens")
  # Default location of the screens tools/swfdump translated out of the games'
  # own Scaleform movies. RX_VANILLA_UI names which ones to load, RX_VANILLA_UI_DIR
  # overrides this path.
  target_compile_definitions(recreation PRIVATE
    RECREATION_VANILLA_UI_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/ui/vanilla")
  recreation_embed_shaders(recreation
    ui/shaders/ugui_quad.vs.hlsl
    ui/shaders/ugui_quad.ps.hlsl
    ui/shaders/ugui_text.vs.hlsl
    ui/shaders/ugui_text.ps.hlsl
    ui/shaders/ugui_frost.vs.hlsl
    ui/shaders/ugui_frost.ps.hlsl
    ui/shaders/thumb.vs.hlsl
    ui/shaders/thumb.ps.hlsl
  )
endif()

# Dedicated server: same engine, headless config baked in. Still links the
# renderer module since the engine owns one; it never initializes vulkan.
add_executable(recreation-server app/server_main.cc ${RECREATION_RUNTIME_SOURCES})
target_link_libraries(recreation-server PRIVATE ${RECREATION_RUNTIME_MODULES})
recreation_set_warnings(recreation-server)
recreation_enable_mimalloc(recreation-server)

if(RECREATION_NET)
  target_link_libraries(recreation-server PRIVATE recreation::gamenet rx::net_viz)
  target_compile_definitions(recreation-server PRIVATE RECREATION_HAS_NET=1)
endif()

# Component tests, beside the code they exercise. Each is a plain executable
# over the pure-data parts of a runtime component, so none of them boots the
# engine or needs a window.
if(RECREATION_BUILD_TOOLS)
  # The translated Scaleform screens have to be markup ultragui accepts; this
  # parses whatever tools/swfdump produced and passes trivially when a checkout
  # has none (they are derived from an installed game, so they are not in tree).
  if(RECREATION_HAS_UGUI)
    recreation_add_test(ui/vanilla_uitest ultragui)
    target_compile_definitions(vanilla_uitest PRIVATE
      RECREATION_VANILLA_UI_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/ui/vanilla")
  endif()
  recreation_add_test(demo/trailertest recreation::weather rx::core SRCS demo/trailer.cc)
  recreation_add_test(editor/editor_layouttest rx::core)
  recreation_add_test(character/chargen_presettest rx::core)
endif()

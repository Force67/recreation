# ui

Hides **everything drawn on top of the scene**.

- `game_ui`: the player-facing HUD and menus, emitted as libultragui draw data.
  The editable screen fragments live in `screens/` and hot-reload.
- `debug_ui`: the developer imgui overlay (render settings, quest browser,
  profilers). Never shipped to the player.
- `gui_backend`, `ugui_platform`, `ugui_rhi`, `ugui_script_csharp`: the ugui
  backends recreation supplies; they are compiled into libultragui itself, and
  `ugui_csharp_host.h` is the host-agnostic seam to the managed runtime.
- `shader_pack` and `shaders/`: the HUD/thumbnail shaders recreation owns.
- `platform_hud`: the multiplayer platform's chat/prompt/menu channel.
- `thumbnailer`: offscreen model renders for the menu and asset browser.

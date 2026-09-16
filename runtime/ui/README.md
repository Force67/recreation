# ui

Hides **everything drawn on top of the scene**.

- `game_ui`: the player-facing HUD and menus, emitted as libultragui draw data.
  The editable screen fragments live in `screens/` and hot-reload.
- `vanilla_ui`: loads screens `tools/swfdump` translated out of the games' own
  Scaleform movies (`RX_VANILLA_UI`), binding their SVG and PNG art back onto
  the image widgets the manifest names.
- `debug_ui`: the developer imgui overlay (render settings, quest browser,
  profilers). Never shipped to the player.
- `gui_backend`: the Vulkan renderer for the HUD's ugui draw data. rx::ui has
  the same backend for the engine splash; this one stays for its growing
  descriptor pools, which a vanilla Scaleform screen's art needs.
- `ugui_script_csharp`: compiled into libultragui itself, it runs ugui handlers
  in the hosted .NET world instead of Lua; `ugui_csharp_host.h` is the
  host-agnostic seam to the managed runtime. The platform and RHI shims beside
  it are rx's (`rx::ui`), which is also where the ugui pipeline shaders live.
- `shader_pack` and `shaders/`: the thumbnail shaders recreation owns.
- `platform_hud`: the multiplayer platform's chat/prompt/menu channel.
- `thumbnailer`: offscreen model renders for the menu and asset browser.

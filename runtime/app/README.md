# app

The **composition root**: it constructs the subsystems, wires them together and
runs the loop. It is the one place allowed to know about everything.

- `main.cc`, `server_main.cc`, `android_main.cc`: the three deliverables' entry
  points, each a thin shell over `Engine`.
- `engine.h/.cc`: the `app::Application` subclass that owns the subsystems.
  Its methods are split across sibling translation units by topic
  (`frame_loop`, `content_load`, `camera_input`, `main_menu`, `first_run`,
  `controls_settings`, `networking`, `managed_rpc`, `managed_scripting`).
- `engine_context.h`: `EngineConfig` plus the `EngineContext` service pointers
  the subsystems read through.
- `content_domain`: an additional game loaded beside the primary one, with its
  own isolated Papyrus microvm.

> **Known debt.** `Engine` owns every subsystem and `EngineContext` is passed to
> most of them, so both are god objects by the code-structure rules: a unit that
> needs the renderer should take the renderer, not the whole world. Splitting
> `EngineContext` into domain-scoped structs is the next structural step; do not
> add members to it in the meantime.

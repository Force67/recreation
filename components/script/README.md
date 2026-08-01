# script

Hides **the scripting runtimes the games ship with**, behind one `script_system`
the runtime drives.

- `papyrus/`: the Skyrim/Fallout 4 VM. `pex` loads bytecode, `interpreter` and
  `vm` execute it, `fiber` + `fiber_scheduler` give latent calls (Wait) real
  coroutines, `transpile`/`decompiler` are the experimental C# path.
- `obscript/`: the Fallout 3 / New Vegas source-level interpreter (those games
  ship SCPT source text, not bytecode).
- `games/skyrim/`: the native function surface, split by subject.
- `host/`: the CoreCLR host for C# mods and the host<->guest bridge.

`world_effect_sink.h` is the seam: script code states world mutations through
this interface and never reaches into the ECS.

# Changelog

All notable changes to Recreation are documented here. The main menu's NEWS rail
shows the most recent entries (the first bullet of each release is its headline).

## [Unreleased]
### Added
- More of the Skyrim native surface the base-game scripts call. This batch
  covers the natives that compute a real value from data the engine already has:
  Game.GetSunPosition X/Y/Z from the game clock, Game.GetGameSettingInt, the
  Utility.GameTimeToString clock formatter, the Utility frame-rate getters, and
  Actor.GetActorValueMax plus the ObjectReference container queries
  (IsContainerEmpty, GetAllItemsCount, RemoveAllItems) composed from the existing
  inventory bindings. New batches live in their own `skyrim_natives_*.cc` files
  and are covered by `nativesexttest`. `tools/papyrus/nativescan` reports the
  coverage, now 169 of 686 declared natives; the remainder drive engine systems
  (animation, movement, combat, equipment) that are still to come.

### Experimental
- Papyrus to C# decompiler (`tools/papyrus/pex2cs`). It pulls a shipped quest
  script out of the game archives and recompiles its bytecode into recreation-SDK
  C#, so a quest can be re-edited as readable C# instead of hand-ported. The
  reconstruction inlines the compiler's temporaries into expressions, hoists the
  temps that span blocks, and rebuilds property, array, and call syntax. It
  structures if, else-if, while, break, and continue control flow, dedups
  auto-property backing fields, state-qualifies overrides, folds Papyrus's
  case-insensitive identifiers onto one spelling, and emits the casts and
  trailing returns Papyrus leaves implicit. Flow it cannot structure falls back
  to a tagged labelled-goto rendering. The decompiler lives behind
  `TranspileToCSharp` and the body reconstruction in `decompiler.cc`, with
  `transpiletest` covering the cases above.

  Validation ran over the full Skyrim SE script set, all 14,302 scripts. Every
  script parses and reconstructs as structured C# with no goto fallback and no
  unmodelled opcodes (`pex2cs --audit`). Parsed by Roslyn, the output has zero
  syntax errors, and the whole corpus compiles as one assembly with a stock C#
  compiler (`pex2cs --compile-check`, engine types collapsed to `dynamic`), which
  checks scoping, definite returns, and break or continue placement. Beyond
  compiling, `pex2cs --difftest` runs every pure function in both the Papyrus VM
  and the compiled C# over identical inputs; all 21,856 trials match, which
  surfaced and fixed a short-circuit bug where a reused temp slot has to share one
  C# local across branches. `pex2cs --runtest` then runs a quest's stage
  fragments in both, logging every engine call with its arguments. On the first
  main quest, MQ101, all 159 fragments run to completion with no primitive
  argument mismatch, for instance Fragment_32 calling SetOpen(false) then
  SetLockLevel(5). That argument check is what surfaced and fixed dropped casts,
  so an int argument to a float or bool parameter now keeps its converted type.

## [0.5.0] - 2026-06-29
### Added
- Game audio is live: an SDL3-backed mixer plays the worlds' sound
- Software mixer with 3D positional voices (distance attenuation + constant-power
  panning around the player), streaming sources, looping, and click-free fades
- Native decoder for the games' WAV assets (PCM 8/16/24/32-bit, IEEE float, and
  the MS and IMA ADPCM codecs), with no external dependencies
- Compressed formats decode through an optional FFmpeg backend, off by default and
  shipped as separate shared libraries: xWMA (Skyrim/Fallout 4 music and ambience),
  the WMA inside FUZ voice files, and Starfield's Wwise .wem
- Region ambience: walking into an area cross-fades to the bed its REGN record
  authors, resolved through the game's SOUN/SNDR sound files; indoors falls silent
- Suppression and level controls: REC_AUDIO_MUTE (base::Option "audio.mute") opens
  no device and runs silent, REC_AUDIO_VOLUME ("audio.volume") sets the master level

## [0.4.0] - 2026-06-23
### Added
- FiveM-style asset streaming: a server distributes its mods to players on join
- The host catalogs a mods directory (--mods-dir); clients pull only the content
  they are missing into a content-addressed cache, then mount it into the asset Vfs
- Authors iterate live: SIGHUP reloads the server's mods without a restart, and
  connected players receive the change live (re-diff, stream only what changed,
  re-mount, no rejoin); modtool inspects what a mods directory will stream
- Scripting RPC channel woven into multiplayer: C# mods emit and receive calls
  (Rpc.Emit / Rpc.ToClient / Rpc.Broadcast / Rpc.On) over the session, plus
  ask-and-answer request/response (Rpc.Request / Rpc.OnRequest / req.Reply)
- A ClientAssetsReady event fires server-side once a player finished downloading,
  so mods can gate spawn or greet the player when their UGC has arrived
- Mod realms ([Realm] Server/Client/Shared): connecting clients run client-side
  mods (RPC, UI, local effects) while authoritative gameplay stays server-side
- Multiplayer lifecycle events for server-side scripts: ClientJoined,
  ClientAssetsReady and ClientLeft

## [0.3.0] - 2026-06-23
### Added
- Cinematic main menu is live
- Full-bleed three-pane front screen with a procedural 3D hero object per universe
- Per-universe atmospheric backdrops generated at runtime, no external art
- Functional social/settings shortcuts on the bottom bar
### Fixed
- UI canvas tracks the swapchain extent, fixing the half-screen black bar
- Cursor mapping scaled to the backbuffer so menu clicks land precisely

## [0.2.0] - 2026-05-30
### Added
- Cross-game NPC reaction layer
- Wanted levels, guard response, daily routines and bystander alarm
- Damage resistance, sneak/crit and legendary layers for Fallout 4 and Starfield
### Changed
- Quest-driven world edits now replicate across multiplayer sessions

## [0.1.0] - 2026-04-18
### Added
- Native quest graph engine
- Quest graph IR with a condition layer, a superset of Skyrim QUST
- C# scripting host bridged to the ultragui runtime
- Placed NPCs load as ECS entities with authoritative client activation

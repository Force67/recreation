# Changelog

All notable changes to Recreation are documented here. The main menu's NEWS rail
shows the most recent entries (the first bullet of each release is its headline).

## [Unreleased]
### Added
- The in-game map editor now sculpts Bethesda LAND non-destructively with live
  raise, lower, smooth, and flatten strokes. Sparse canonical-border deltas save
  as checksummed, source-fingerprinted `.recterrain` files; streamed meshes,
  ground queries, and colliders refresh per touched cell, and a whole drag is one
  undo operation.
- Quest scripts now react to more of the world through engine-raised Papyrus
  events. Combat drives `OnHit` (with the aggressor) and `OnCombatStateChanged` as
  an actor enters and leaves combat, including when its target dies. Stepping out
  of a scripted trigger box raises `OnTriggerLeave`, matching `OnTriggerEnter`.
  Equipping or removing gear raises `OnObjectEquipped`/`OnObjectUnequipped` on the
  actor and `OnEquipped`/`OnUnequipped` on the item; `IsEquipped` and
  `GetEquippedWeapon`/`GetEquippedShield` report what an actor is wearing,
  classified from the record. Every engine-raised event (activation, triggers,
  equip) also reaches the scripts on the quest aliases a reference fills, the way a
  filled actor's death already did, so alias-driven objectives fire. Covered by
  `pexrun selftest`/`guesttest` and the record-backed equip getters by
  `bindingstest`.
- Registered every Skyrim native the base-game scripts call, all 686 of the 686
  that `tools/papyrus/nativescan` finds. Each one is implemented as faithfully as
  the engine allows. Functions that read record data return the real value:
  FormList.GetSize/GetAt/Find/HasForm read the FLST list, item hostility comes
  from a detrimental magic effect, and the spell, weapon, and actor-value getters
  route to the record bindings. Functions that carry runtime state round-trip
  through a shared keyed store, so SetGhost then IsGhost, the angle setters and
  getters, owner and faction-owner, perks, and the crime-gold split all report
  what a script set. The remaining functions drive engine systems that are not
  built yet (animation, movement, combat, weather, audio); they are wired so the
  scripts run, and they become real as those systems land. The batches live in
  their own `skyrim_natives_*.cc` files behind a shared keyed state store, with
  the computed and stateful paths covered by `nativesexttest`.

### Fixed
- The optional default gamemodes (the per-game rulesets, each its own assembly)
  failed to load: mods were loaded into the wrong assembly load context, so a
  gamemode could not bind the SDK already in memory and its systems never
  installed. Mods now load into the SDK's own context and share its types, so the
  Skyrim, Fallout, and Starfield rulesets (attribute regeneration and the rest)
  come online as intended, verified end to end by `papyrus_managed_hosttest`.

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
- Suppression and level controls: RX_AUDIO_MUTE (base::Option "audio.mute") opens
  no device and runs silent, RX_AUDIO_VOLUME ("audio.volume") sets the master level

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

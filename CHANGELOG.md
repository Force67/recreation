# Changelog

All notable changes to Recreation are documented here. The main menu's NEWS rail
shows the most recent entries (the first bullet of each release is its headline).

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

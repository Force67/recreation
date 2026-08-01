# actor

Hides **the living things in the world and how they move**.

- `actor_system`: spawning actors from NPC_/ACHR records, their body rig,
  skeleton and animation, and the walkable player actor.
- `player_controller`: walk-mode locomotion, stance and the camera-relative
  move verbs; `gait_rate` is the anti-foot-slide playback clock.
- `nav_bubble`: the exterior navmesh kept streamed around the player, and the
  per-actor corridors actors walk through it.
- `npc_director`: what NPCs decide to do (follow, flee, fight, go somewhere).
- `ai_package_director`: runs the PACK records that schedule those decisions.

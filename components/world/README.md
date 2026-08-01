# world

Hides **how a Bethesda worldspace becomes live engine state**. Records go in,
ECS entities, meshes, colliders and navigation come out.

- Streaming: `cell_streaming` (the exterior/interior cell window),
  `prop_streaming` (placed references), `planet_tile` (Starfield procedural).
- Ground: `land_baker` and `grass_baker` turn LAND/GRAS into meshes and splat
  weights; `terrain_edits` is the non-destructive height-edit layer
  (see `TERRAIN_EDITS.md`).
- Navigation and movement: `navgrid`, `pathfind`, `steering_avoidance`,
  `npc_ai`, `carriage_rig`.
- Mirrors read by other components: `quest_world`, `combat`, `interaction`,
  `objective_marker`, `components.h`, plus `cine_camera` for scripted shots.

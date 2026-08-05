# world

Hides **how a Bethesda worldspace becomes live engine state**. Records go in,
ECS entities, meshes, colliders and navigation come out.

- Streaming: `cell_streaming` (the exterior/interior cell window),
  `prop_streaming` (placed references), `planet_tile` (Starfield procedural).
- Ground: `land_baker` and `grass_baker` turn LAND/GRAS into meshes and splat
  weights; `terrain_edits` is the non-destructive height-edit layer.
- Navigation and movement: `navgrid`, `pathfind`, `steering_avoidance`,
  `npc_ai`, plus the carriage trio: `carriage_records` (the linked references a
  hold carriage is built from, and the journeys its horse is given),
  `carriage_rig` (the towed-cart physics) and `cart_wheels` (cutting the wheels
  out of a cart's art so they can turn).
- Mirrors read by other components: `quest_world`, `combat`, `interaction`,
  `objective_marker`, `components.h`, plus `cine_camera` for scripted shots.

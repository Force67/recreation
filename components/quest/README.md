# quest

Hides **the QUST/SCEN record model and the state machine that runs it**. Pure
state: no ECS, no renderer, no script VM, which is why it is testable with no
game data.

- Definitions: `quest_def`, `quest_import`, `quest_graph`, `package_record`.
- Runtime: `quest_system` (stages, objectives, aliases).
- Conditions: `ctda` decodes the record form, `condition` is the engine-side IR
  other components evaluate against their own world view.
- Scenes: `scene_record` -> `scene_compile` -> `scene_player` / `scene_runtime`,
  the phase/action timeline behind cutscenes.

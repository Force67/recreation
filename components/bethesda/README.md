# bethesda

Hides **Bethesda's on-disk formats**. Everything above this component works in
engine types and never parses a byte of game data itself.

- Plugins and records: ESM/ESP/ESL (`plugin`, `record`, `tes3`, `load_order`,
  `form_id`, `strings`), plus the write path (`writer`, `edit_session`,
  `raw_rewriter`, `string_writer`, see `WRITE_ARCHITECTURE.md`).
- Archives: BSA and BA2 v1/v2/v3 behind one `archive` interface.
- Meshes and animation: NIF (Gamebryo through Starfield's external `.mesh`),
  HKX skeletons/clips/ragdolls and their conversion to kinema and to physics,
  `.tri` face morphs, FaceGen, movement types.
- Material and world data: `material_db` (`materialsbeta.cdb`), `biom`, `planet`.

One decision per file, and the reader/writer pair for a format lives together.

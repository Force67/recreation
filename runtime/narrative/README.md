# narrative

Hides **staged storytelling**: the parts of the game that take control away from
the player and play out a written sequence.

- `cutscene_director`: SCEN playback, the dialogue camera and voice pacing.
- `helgen_intro`: the Skyrim opening, driven from the records' AI package chain.
- `quest_director`: drives quest progression, objectives and markers, and
  `quest_state_cache` is the snapshot the HUD and debugger read.

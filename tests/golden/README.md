# Golden-image regression tests

`golden.py` renders the demo scenes headlessly and compares frame captures
against the references in `refs/`. `RX_FIXED_DT` locks the frame delta, so
every time-driven system (water waves, particles, exposure adaptation,
day/night sun) is a pure function of the frame index and captures are
deterministic on a given driver stack.

```sh
# compare (GB10 / NVIDIA, from the nix dev shell):
nix develop -c python3 tests/golden/golden.py --runner vkrun

# generate or regenerate the local refs:
nix develop -c python3 tests/golden/golden.py --runner vkrun --update
```

Failures write `<scene>_diff.png` heatmaps next to the captures in
`build/golden/`.

Notes:

- `--binary` defaults to `build/linux/runtime/recreation` (the CMakePresets
  "linux" preset output); pass `--binary` to point at another build dir.
- Captures are pinned to 1920x1008 (`RX_WIN_W`/`RX_WIN_H` are set with
  `setdefault`, so the refs' geometry is the default but an explicit env
  override still wins) so a WM that hands the window a different client size
  does not fail every scene as a size mismatch.
- References are driver-stack-specific and are not checked in; generate a
  local set with `--update` (stored at half resolution). Scenes without a
  reference run smoke-only (crash / black-frame detection) instead of failing.
- CI runs the smoke mode on lavapipe (`golden-smoke` job in build.yml),
  uploading captures as artifacts. Promote a runner capture set with
  `--update` to turn CI comparisons on, then drop the job's
  `continue-on-error`.
- lavapipe from the nix shell (`swrun`) currently segfaults on aarch64
  (mesa 26.1.2 JIT crash, reproduces on pre-Tier-1 commits too); use vkrun
  on the GB10 until a mesa bump fixes it.

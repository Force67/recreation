# Feature-Gym Regression Tour

The tour runs `--demo featuregym` with a fixed timestep, captures all 28 labeled
camera stops, and rejects missing, black, or uniform frames. Alongside the nine
district sweeps, dedicated stops cover recent RX instance streaming, material,
terrain, virtual-geometry, water, strand, interest-bubble, and camera-stack APIs.

```sh
python3 tests/feature_gym/tour.py --runner vkrun
```

Add `--no-rt` to run the RCGI stop through the software SDF tracer.

Use `--binary`, `--out`, and `--timeout` to point at another build or GPU runner.
The path-tracing and vendor-upscaler stops deliberately exercise RX's capability
fallbacks when the active GPU does not provide those features.

The harness requires Pillow (`python3-pil` or the `Pillow` Python package). It
checks exact labels, capture count, 1280x720 dimensions, and basic image variance;
feature-specific engine behavior remains covered by RX's focused unit tests.

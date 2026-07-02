#!/usr/bin/env python3
"""Golden-image regression harness.

Renders a fixed set of demo scenes headlessly (REC_FIXED_DT locks animation
to the frame index, so a capture at frame N is deterministic) and compares
the captures against checked-in reference images.

Typical use:
  # compare against refs (GB10 / NVIDIA via the nix dev shell):
  nix develop -c python3 tests/golden/golden.py --runner vkrun

  # after an intentional rendering change, regenerate the references:
  nix develop -c python3 tests/golden/golden.py --runner vkrun --update

  # CI smoke mode (no refs for the platform): still fails on crashes,
  # missing captures or degenerate (black/uniform) images:
  python3 tests/golden/golden.py --profile loose

Reference sets are per-GPU-stack: images from different drivers/rasterizers
are not comparable at golden tolerances. --refs selects the set; the default
tests/golden/refs holds the primary (NVIDIA vkrun) baseline. A scene without
a reference runs in smoke mode instead of failing, so a fresh platform can
bootstrap by uploading its captures and promoting them with --update.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# name, demo, capture frame, extra env. Frame 90 gives the temporal
# pipeline (TAA/FSR, froxel reprojection, exposure adaptation) time to settle.
SCENES = [
    ("bricks", "bricks", 90, {}),   # pom, decals, local shadows, glossy floor
    ("lights", "lights", 90, {}),   # clustered + shadowed local lights, froxel
    ("water", "water", 90, {}),     # animated water, wboit, particles
    ("fire", "fire", 90, {}),       # emissive particles, froxel scattering
]

PROFILES = {
    # Same machine + driver: run-to-run noise is ~0.001 mean, so anything
    # visible is a real change.
    "strict": {"mean": 1.5, "px_thresh": 8, "px_frac": 0.010},
    # Cross-driver / mesa-version drift (CI software rasterizer).
    "loose": {"mean": 4.0, "px_thresh": 24, "px_frac": 0.020},
}


def run_scene(binary, runner, name, demo, frames, extra_env, out_path, timeout):
    env = os.environ.copy()
    env.update({
        "REC_UI_SHOT": str(out_path),
        "REC_UI_SHOT_FRAMES": str(frames),
        "REC_HIDE_DEBUG_UI": "1",
        "REC_FIXED_DT": "0.016666667",
    })
    env.update(extra_env)
    cmd = ([runner] if runner else []) + [str(binary), "--demo", demo]
    try:
        proc = subprocess.run(cmd, env=env, cwd=REPO, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        return f"TIMEOUT after {timeout}s"
    if not out_path.exists():
        tail = proc.stdout.decode(errors="replace").splitlines()[-15:]
        return "no capture written (rc=%d)\n  %s" % (proc.returncode, "\n  ".join(tail))
    return None


# References are stored at half the capture resolution: a full-res PNG set is
# ~12 MB per update (permanent git history); halving cuts that ~4x while
# feature-level regressions (missing pass, tint shift, broken shadows) still
# read clearly through the 2x2 average.
REF_SCALE = 2


def load(path, downscale=1):
    from PIL import Image
    import numpy as np
    img = Image.open(path).convert("RGB")
    if downscale > 1:
        img = img.resize((img.width // downscale, img.height // downscale),
                         Image.LANCZOS)
    return np.asarray(img).astype(np.float32)


def smoke_check(img):
    """A capture with real content: not black, not uniform."""
    if img.mean() < 2.0:
        return "image is (near) black"
    if img.std() < 2.0:
        return "image is uniform (std %.2f)" % img.std()
    return None


def compare(img, ref, prof, diff_path):
    import numpy as np
    if img.shape != ref.shape:
        return "size mismatch: capture %s vs ref %s (different display setup?)" % (
            img.shape[:2], ref.shape[:2])
    diff = np.abs(img - ref)
    mean = float(diff.mean())
    frac = float((diff.max(axis=2) > prof["px_thresh"]).mean())
    if mean <= prof["mean"] and frac <= prof["px_frac"]:
        return None
    from PIL import Image
    heat = np.clip(diff.max(axis=2) * 4.0, 0, 255).astype("uint8")
    Image.fromarray(heat).save(diff_path)
    return "mean diff %.3f (limit %.1f), %.3f%% pixels > %d (limit %.1f%%); heatmap: %s" % (
        mean, prof["mean"], frac * 100, prof["px_thresh"], prof["px_frac"] * 100, diff_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=str(REPO / "build/nix/runtime/recreation"))
    ap.add_argument("--runner", default="", help="wrapper command (vkrun / swrun)")
    ap.add_argument("--refs", default=str(Path(__file__).parent / "refs"))
    ap.add_argument("--out", default=str(REPO / "build/golden"))
    ap.add_argument("--scenes", default="", help="comma-separated subset")
    ap.add_argument("--profile", default="strict", choices=sorted(PROFILES))
    ap.add_argument("--update", action="store_true", help="promote captures to references")
    ap.add_argument("--timeout", type=int, default=300, help="per-scene seconds")
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        print(f"binary not found: {binary}", file=sys.stderr)
        return 2
    refs = Path(args.refs)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    prof = PROFILES[args.profile]
    subset = {s for s in args.scenes.split(",") if s}

    failures, smoked = [], []
    for name, demo, frames, extra_env in SCENES:
        if subset and name not in subset:
            continue
        capture = out / f"{name}.png"
        capture.unlink(missing_ok=True)
        err = run_scene(binary, args.runner, name, demo, frames, extra_env, capture,
                        args.timeout)
        if err:
            print(f"[FAIL] {name}: {err}")
            failures.append(name)
            continue
        img = load(capture)
        err = smoke_check(img)
        if err:
            print(f"[FAIL] {name}: {err}")
            failures.append(name)
            continue

        ref_path = refs / f"{name}.png"
        if args.update:
            refs.mkdir(parents=True, exist_ok=True)
            from PIL import Image
            ref_img = Image.open(capture).convert("RGB")
            ref_img = ref_img.resize((ref_img.width // REF_SCALE,
                                      ref_img.height // REF_SCALE), Image.LANCZOS)
            ref_img.save(ref_path, optimize=True)
            print(f"[UPDATED] {name} -> {ref_path}")
            continue
        if not ref_path.exists():
            print(f"[SMOKE] {name}: no reference for this platform (capture ok)")
            smoked.append(name)
            continue
        err = compare(load(capture, REF_SCALE), load(ref_path), prof,
                      out / f"{name}_diff.png")
        if err:
            print(f"[FAIL] {name}: {err}")
            failures.append(name)
        else:
            print(f"[PASS] {name}")

    if smoked:
        print(f"{len(smoked)} scene(s) ran smoke-only; promote with --update to pin them.")
    if failures:
        print(f"{len(failures)} scene(s) failed: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

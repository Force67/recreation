#!/usr/bin/env python3
"""Run the deterministic RX feature-gym camera tour and smoke-check every stop."""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
EXPECTED = {
    "overview",
    "materials",
    "split_pbr_per_submesh_materials",
    "terrain_splat_v2",
    "lighting_raster",
    "lighting_raytraced",
    "lighting_rcgi_hw_or_sdf",
    "geometry_scalability",
    "streamed_instance_lifecycle",
    "virtual_geometry_projected_albedo",
    "weather_rain",
    "weather_snow_aurora",
    "water_ocean",
    "gerstner_shoreline_buoyancy",
    "particles_transparency_hair",
    "jolt_procedural_strand_groom",
    "physics_vehicles_character",
    "transport_free_network_bubbles",
    "animation_skin_morph_audio",
    "ecs_camera_stack_rig",
    "post_taa",
    "post_msaa",
    "post_fsr3_fallback",
    "post_dlss_fallback",
    "post_xess_fallback",
    "post_tonemap_grade",
    "interior_fog",
    "path_trace_reconstruction",
}


def capture_label(path: Path) -> str:
    stem = path.stem
    return stem.split("_", 1)[1] if "_" in stem else stem


def smoke_check(path: Path) -> str | None:
    from PIL import Image, ImageStat

    image = Image.open(path).convert("RGB")
    if image.size != (1280, 720):
        return f"wrong dimensions {image.size[0]}x{image.size[1]}"
    stats = ImageStat.Stat(image)
    mean = sum(stats.mean) / 3.0
    deviation = sum(stats.stddev) / 3.0
    if mean < 2.0:
        return "near-black image"
    if deviation < 2.0:
        return f"uniform image (stddev {deviation:.2f})"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default=str(REPO / "build/linux/runtime/recreation"))
    parser.add_argument("--runner", default="", help="wrapper command, for example vkrun")
    parser.add_argument("--out", default=str(REPO / "build/feature-gym-tour"))
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--no-rt", action="store_true",
                        help="exercise RCGI through the software SDF tracer")
    args = parser.parse_args()

    try:
        import PIL  # noqa: F401
    except ImportError:
        print("Pillow is required: install python3-pil or pip install Pillow", file=sys.stderr)
        return 2

    binary = Path(args.binary)
    if not binary.exists():
        print(f"binary not found: {binary}", file=sys.stderr)
        return 2
    output = Path(args.out)
    output.mkdir(parents=True, exist_ok=True)
    for old in output.glob("*.png"):
        old.unlink()

    env = os.environ.copy()
    env.update({
        "RX_FIXED_DT": "0.016666667",
        "RX_WIN_W": "1280",
        "RX_WIN_H": "720",
        "RX_AUDIO_MUTE": "1",
    })
    command = shlex.split(args.runner) + [
        str(binary),
        "--demo", "featuregym",
        "--feature-tour",
        "--feature-tour-shots", str(output),
        "--feature-tour-quit",
    ]
    if args.no_rt:
        command.append("--no-rt")
    try:
        process = subprocess.run(command, cwd=REPO, env=env, timeout=args.timeout,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        print(f"feature gym timed out after {args.timeout}s", file=sys.stderr)
        return 1

    captures = sorted(output.glob("*.png"))
    capture_labels = [capture_label(path) for path in captures]
    labels = set(capture_labels)
    missing = sorted(EXPECTED - labels)
    unexpected = sorted(labels - EXPECTED)
    duplicates = sorted(label for label in labels if capture_labels.count(label) > 1)
    failures = []
    for path in captures:
        error = smoke_check(path)
        if error:
            failures.append(f"{path.name}: {error}")

    if process.returncode != 0:
        log = process.stdout.decode(errors="replace").splitlines()[-30:]
        failures.append(f"process returned {process.returncode}\n  " + "\n  ".join(log))
    if missing:
        failures.append("missing captures: " + ", ".join(missing))
    if unexpected:
        failures.append("unexpected captures: " + ", ".join(unexpected))
    if duplicates:
        failures.append("duplicate captures: " + ", ".join(duplicates))
    if len(captures) != len(EXPECTED):
        failures.append(f"expected {len(EXPECTED)} captures, found {len(captures)}")
    if failures:
        for failure in failures:
            print(f"[FAIL] {failure}", file=sys.stderr)
        return 1
    print(f"[PASS] {len(captures)} feature-gym captures in {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

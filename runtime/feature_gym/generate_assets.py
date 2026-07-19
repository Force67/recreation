#!/usr/bin/env python3
"""Generate deterministic, dependency-free RX feature-gym test assets."""

from __future__ import annotations

import math
import struct
import wave
from pathlib import Path


SIZE = 128
OUT = Path(__file__).resolve().parent / "assets"


def clamp_byte(value: float) -> int:
    return max(0, min(255, round(value * 255.0)))


def write_rgba(name: str, pixel) -> None:
    data = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            data.extend(clamp_byte(channel) for channel in pixel(x, y))
    (OUT / name).write_bytes(data)


def brick_height(u: float, v: float) -> float:
    row = math.floor(v * 8.0)
    x = (u * 4.0 + (0.5 if row & 1 else 0.0)) % 1.0
    y = (v * 8.0) % 1.0
    edge = min(min(x, 1.0 - x) / 0.07, min(y, 1.0 - y) / 0.11, 1.0)
    return edge * edge * (3.0 - 2.0 * edge)


def generate_textures() -> None:
    def checker(x: int, y: int):
        cell = 16
        dark = ((x // cell) + (y // cell)) & 1
        value = 0.22 if dark else 0.58
        if x % cell < 2 or y % cell < 2:
            value = 0.08
        if x < cell and y < cell:
            return (0.95, 0.32, 0.05, 1.0)
        return (value * 0.86, value * 0.94, value, 1.0)

    def albedo(x: int, y: int):
        u = (x + 0.5) / SIZE
        v = (y + 0.5) / SIZE
        band = 0.5 + 0.5 * math.sin((u * 5.0 + v * 3.0) * math.tau)
        return (0.12 + 0.72 * u, 0.18 + 0.65 * v, 0.28 + 0.45 * band, 1.0)

    def tangent_normal(x: int, y: int):
        u = (x + 0.5) / SIZE
        v = (y + 0.5) / SIZE
        dx = math.cos(u * math.tau * 8.0) * 0.34
        dy = math.sin(v * math.tau * 8.0) * 0.34
        nz = math.sqrt(max(0.05, 1.0 - dx * dx - dy * dy))
        length = math.sqrt(dx * dx + dy * dy + nz * nz)
        return (dx / length * 0.5 + 0.5, dy / length * 0.5 + 0.5,
                nz / length * 0.5 + 0.5, 1.0)

    def object_normal(x: int, y: int):
        u = (x + 0.5) / SIZE
        v = (y + 0.5) / SIZE
        phi = v * math.pi
        theta = u * math.tau
        nx = math.sin(phi) * math.cos(theta)
        ny = math.cos(phi)
        nz = math.sin(phi) * math.sin(theta)
        return (nx * 0.5 + 0.5, ny * 0.5 + 0.5, nz * 0.5 + 0.5, 1.0)

    def orm(x: int, y: int):
        u = (x + 0.5) / SIZE
        v = (y + 0.5) / SIZE
        ao = 0.35 + 0.65 * (1.0 - abs(v * 2.0 - 1.0))
        return (ao, 0.04 + 0.92 * u, 1.0 if y < SIZE // 2 else 0.0, 1.0)

    def roughness(x: int, y: int):
        u = (x + 0.5) / SIZE
        v = (y + 0.5) / SIZE
        value = 0.08 + 0.84 * (0.65 * u + 0.35 * (0.5 + 0.5 * math.sin(v * math.tau * 4.0)))
        return (0.0, value, 0.0, 1.0)

    def metallic(x: int, y: int):
        u = (x + 0.5) / SIZE
        v = (y + 0.5) / SIZE
        value = 0.12 + 0.88 * (1.0 if ((x // 16) + (y // 16)) & 1 else u * v)
        return (value, 0.0, 0.0, 1.0)

    def ambient_occlusion(x: int, y: int):
        u = (x + 0.5) / SIZE - 0.5
        v = (y + 0.5) / SIZE - 0.5
        value = 0.28 + 0.72 * min(1.0, math.hypot(u, v) * 2.4)
        return (value, value, value, 1.0)

    def height(x: int, y: int):
        h = brick_height((x + 0.5) / SIZE, (y + 0.5) / SIZE)
        return (h, h, h, 1.0)

    def alpha(x: int, y: int):
        u = (x + 0.5) / SIZE - 0.5
        v = (y + 0.5) / SIZE - 0.5
        rings = 0.5 + 0.5 * math.sin(math.hypot(u, v) * math.tau * 12.0)
        a = 1.0 if rings > 0.48 and abs(u) < 0.47 and abs(v) < 0.47 else 0.0
        return (0.15, 0.82, 0.44, a)

    def emissive(x: int, y: int):
        u = (x + 0.5) / SIZE - 0.5
        v = (y + 0.5) / SIZE - 0.5
        line = max(math.exp(-abs(u) * 42.0), math.exp(-abs(v) * 42.0))
        ring = math.exp(-abs(math.hypot(u, v) - 0.28) * 55.0)
        glow = min(1.0, line + ring)
        return (glow, glow * 0.34, glow * 0.06, max(glow, 0.08))

    def terrain_weights(x: int, y: int):
        u = (x + 0.5) / SIZE
        v = (y + 0.5) / SIZE
        centers = ((0.08, 0.12), (0.46, 0.08), (0.84, 0.14), (0.92, 0.54),
                   (0.78, 0.88), (0.42, 0.92), (0.10, 0.78), (0.28, 0.48))
        values = []
        for i, (cx, cy) in enumerate(centers):
            distance = (u - cx) ** 2 + (v - cy) ** 2
            values.append(math.exp(-distance * (13.0 + (i % 3) * 2.0)) + 0.015)
        total = sum(values)
        return tuple(value / total for value in values)

    def weights(x: int, y: int):
        return terrain_weights(x, y)[:4]

    def weights_b(x: int, y: int):
        return terrain_weights(x, y)[4:]

    def decal(x: int, y: int):
        u = (x + 0.5) / SIZE - 0.5
        v = (y + 0.5) / SIZE - 0.5
        angle = math.atan2(v, u)
        radius = 0.36 + 0.07 * math.sin(angle * 7.0) + 0.04 * math.sin(angle * 13.0)
        edge = max(0.0, min(1.0, (radius - math.hypot(u, v)) * 24.0))
        return (0.07 + edge * 0.06, 0.42 + edge * 0.25, 0.68 + edge * 0.22, edge)

    def decal_normal(x: int, y: int):
        u = (x + 0.5) / SIZE - 0.5
        v = (y + 0.5) / SIZE - 0.5
        nx = max(-0.4, min(0.4, -u * 0.9))
        ny = max(-0.4, min(0.4, -v * 0.9))
        nz = math.sqrt(max(0.05, 1.0 - nx * nx - ny * ny))
        return (nx * 0.5 + 0.5, ny * 0.5 + 0.5, nz * 0.5 + 0.5, 1.0)

    write_rgba("checker.rgba", checker)
    write_rgba("albedo.rgba", albedo)
    write_rgba("normal.rgba", tangent_normal)
    write_rgba("object_normal.rgba", object_normal)
    write_rgba("orm.rgba", orm)
    write_rgba("roughness.rgba", roughness)
    write_rgba("metallic.rgba", metallic)
    write_rgba("ao.rgba", ambient_occlusion)
    write_rgba("height.rgba", height)
    write_rgba("alpha.rgba", alpha)
    write_rgba("emissive.rgba", emissive)
    write_rgba("weights.rgba", weights)
    write_rgba("weights_b.rgba", weights_b)
    write_rgba("decal.rgba", decal)
    write_rgba("decal_normal.rgba", decal_normal)


def generate_tone() -> None:
    sample_rate = 48_000
    seconds = 2.0
    frames = int(sample_rate * seconds)
    with wave.open(str(OUT / "spatial_tone.wav"), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        samples = bytearray()
        for i in range(frames):
            t = i / sample_rate
            envelope = min(1.0, t * 8.0, (seconds - t) * 8.0)
            signal = (math.sin(math.tau * 220.0 * t) * 0.34 +
                      math.sin(math.tau * 440.0 * t) * 0.12) * envelope
            samples.extend(struct.pack("<h", round(signal * 32767.0)))
        wav.writeframes(samples)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    generate_textures()
    generate_tone()
    print(f"generated feature-gym assets in {OUT}")


if __name__ == "__main__":
    main()

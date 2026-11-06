#!/usr/bin/env python3
"""Generate the animated WebP fixture for the quantization dataset.

The main synthetic generator uses only the Python standard library. The
standard library has no WebP encoder, so this one fixture is produced with
Pillow's WebP encoder instead. The image content is drawn in code here, so the
result is project-owned (CC0-1.0) and deterministic: no timestamps, no random
values, fixed frame count, fixed duration and fixed encoder method.

Usage:
    py -3.14 scripts/generate_animated_webp.py
    py -3.14 scripts/generate_animated_webp.py --verify

--verify recomputes the file and fails when the bytes on disk differ.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from PIL import Image

WIDTH = 16
HEIGHT = 16
FRAME_COUNT = 2
FRAME_MS = 100
OUTPUT = Path(__file__).resolve().parents[1] / "test" / "data" / "generated" / \
    "multiframe_animated_2frame.webp"


def build_frame(index: int) -> Image.Image:
    """Draw one deterministic RGBA frame with a moving 4x4 block."""
    image = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    pixels = image.load()
    background = (200, 40, 40, 255) if index == 0 else (40, 40, 200, 160)
    block = (255, 255, 255, 255) if index == 0 else (0, 255, 0, 255)
    block_x = 2 + index * 8
    for y in range(HEIGHT):
        for x in range(WIDTH):
            inside = block_x <= x < block_x + 4 and 6 <= y < 10
            pixels[x, y] = block if inside else background
    return image


def encode() -> bytes:
    frames = [build_frame(index) for index in range(FRAME_COUNT)]
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(
        OUTPUT,
        format="WEBP",
        save_all=True,
        append_images=frames[1:],
        duration=FRAME_MS,
        loop=0,
        lossless=True,
        method=6,
    )
    return OUTPUT.read_bytes()


def verify(data: bytes) -> None:
    if data[:4] != b"RIFF" or data[8:12] != b"WEBP":
        raise SystemExit("not a RIFF/WEBP file")
    if b"ANIM" not in data or b"ANMF" not in data:
        raise SystemExit("the animation chunks are missing")
    with Image.open(OUTPUT) as image:
        if image.format != "WEBP" or getattr(image, "n_frames", 1) != FRAME_COUNT:
            raise SystemExit("the decoded frame count is not 2")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()

    previous = OUTPUT.read_bytes() if OUTPUT.exists() else None
    data = encode()
    verify(data)
    digest = hashlib.sha256(data).hexdigest()

    if args.verify:
        if previous != data:
            raise SystemExit("the fixture on disk differs from a fresh run")
        print("VERIFY OK")
    print(f"path={OUTPUT.as_posix()}")
    print(f"dimensions={WIDTH}x{HEIGHT} ({FRAME_COUNT} frames)")
    print(f"byte_size={len(data)}")
    print(f"sha256={digest}")


if __name__ == "__main__":
    main()

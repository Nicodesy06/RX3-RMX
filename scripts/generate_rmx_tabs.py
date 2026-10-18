#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Relabel the native performance tabs for the RMX X-PAD and ISOLATOR.

The tab strip artwork is the firmware's own, captured by the KEY, STEMS and
SAMPLES modules. Their frames, corner radius and fill texture are kept; only
the caption of the half a module takes over is erased and lettered again.
The firmware draws these captions in Helvetica Neue LT W1G, which is licensed
and not redistributable, and the native artwork holds no R, O, C or 2 to
borrow, so the new captions are set in an Arial-metric system face. That is an
approximation and looks like one up close.

    python3 scripts/generate_rmx_tabs.py [--font path/to/font.ttf]
"""

from __future__ import annotations

import argparse
import pathlib
from struct import pack

from PIL import Image, ImageDraw, ImageFont

MODULES = pathlib.Path(__file__).resolve().parents[1] / "mod/modules"
SIZE = (180, 50)

# (owner of the source art, source name, target module, target name, half, caption)
JOBS = (
    ("keyshift", "key-selected", "rmx-looper", "rmx-key-selected", "right", "RMX"),
    ("keyshift", "none-selected", "rmx-looper", "rmx-none-selected", "right", "RMX"),
    ("stems", "stems-selected", "rmx-looper", "rmx-selected", "right", "RMX"),
    ("stems", "single-stems-none", "rmx-looper", "single-rmx-none", "whole", "RMX X-PAD"),
    ("stems", "single-stems-selected", "rmx-looper", "single-rmx-selected", "whole", "RMX X-PAD"),
    ("samples", "samples-selected", "dual-color-fx", "fx2-selected", "left", "COLOR 2"),
    ("samples", "samples-none-selected", "dual-color-fx", "fx2-none-selected", "left", "COLOR 2"),
    ("samples", "samples-beatfx-selected", "dual-color-fx", "fx2-beatfx-selected", "left", "COLOR 2"),
)

FONTS = (
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "C:/Windows/Fonts/arial.ttf",
)


def read(path: pathlib.Path) -> Image.Image:
    return Image.frombytes("RGB", SIZE, path.read_bytes(), "raw", "BGR;16")


def write(path: pathlib.Path, image: Image.Image) -> None:
    pixels = bytearray()
    for red, green, blue in image.get_flattened_data():
        pixels += pack("<H", (red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3))
    path.write_bytes(pixels)


def span(half: str) -> tuple[int, int]:
    return {"left": (4, 89), "right": (91, 176), "whole": (4, 176)}[half]


def luminance(rgb) -> float:
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def relabel(source: Image.Image, half: str, caption: str, font_path: str) -> Image.Image:
    image = source.copy()
    x0, x1 = span(half)
    # The ground is the most common colour inside the half; the ink is the
    # pixel furthest from it, which is the old caption's own colour.
    counts: dict = {}
    for y in range(6, 44):
        for x in range(x0, x1):
            counts[source.getpixel((x, y))] = counts.get(source.getpixel((x, y)), 0) + 1
    ground = max(counts, key=counts.get)
    ink = max(counts, key=lambda c: abs(luminance(c) - luminance(ground)))
    # Erase the old caption with the half's own texture. A pixel is clean when
    # it sits near the ground; every other one takes the nearest clean pixel
    # of its row with the same phase of the fill's 8-pixel dither, so the
    # texture continues under where the letters were.
    contrast = abs(luminance(ink) - luminance(ground)) or 1.0
    for y in range(6, 44):
        clean = [x for x in range(x0 + 1, x1 - 1)
                 if abs(luminance(source.getpixel((x, y))) - luminance(ground)) < 0.12 * contrast]
        for x in range(x0 + 1, x1 - 1):
            if x in clean:
                continue
            same_phase = [c for c in clean if (c - x) % 8 == 0]
            pool = same_phase or clean
            image.putpixel((x, y), source.getpixel((min(pool, key=lambda c: abs(c - x)), y))
                           if pool else ground)
    # Cap height 16 px, as measured on the firmware's captions.
    size = 22
    font = ImageFont.truetype(font_path, size)
    while size > 12:
        width = font.getlength(caption)
        if width <= (x1 - x0) - 8:
            break
        size -= 1
        font = ImageFont.truetype(font_path, size)
    draw = ImageDraw.Draw(image)
    box = draw.textbbox((0, 0), caption, font=font)
    left = x0 + ((x1 - x0) - (box[2] - box[0])) // 2 - box[0]
    top = 25 - (box[3] + box[1]) // 2
    draw.text((left, top), caption, font=font, fill=ink)
    return image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--font")
    arguments = parser.parse_args()
    font = arguments.font or next((f for f in FONTS if pathlib.Path(f).is_file()), None)
    if not font:
        raise SystemExit("no usable font found; pass --font")
    for owner, name, target, output, half, caption in JOBS:
        for suffix in ("", "-light"):
            source = MODULES / owner / "assets" / f"{name}{suffix}.rgb565"
            if not source.is_file():
                continue
            destination = MODULES / target / "assets" / f"{output}{suffix}.rgb565"
            destination.parent.mkdir(parents=True, exist_ok=True)
            write(destination, relabel(read(source), half, caption, font))
            print(destination.relative_to(MODULES.parent.parent))


if __name__ == "__main__":
    main()

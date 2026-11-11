#!/usr/bin/env python3
"""Convert the App's 500px transparent skin PNGs to LVGL 9 RGB565A8."""

from pathlib import Path
import argparse
from PIL import Image

SKINS = ("sleep", "love", "dizzy", "hurt", "cool", "study", "health", "sport")


SIZE = 160


def convert(source: Path, destination: Path) -> None:
    image = Image.open(source).convert("RGBA").resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    rgb565 = bytearray()
    alpha = bytearray()
    for red, green, blue, opacity in image.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        rgb565.extend((value & 0xFF, value >> 8))
        alpha.append(opacity)
    data = rgb565 + alpha
    name = f"brain_{source.stem.removeprefix('brain_')}"
    lines = []
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
    output = f'''#include "brain_images.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

/* Generated from the App's transparent 500px asset. RGB565 + 8-bit alpha. */
static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_data[] = {{
{chr(10).join(lines)}
}};

lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.w = {SIZE},
    .header.h = {SIZE},
    .header.stride = {SIZE * 2},
    .data_size = sizeof({name}_data),
    .data = {name}_data,
}};
'''
    (destination / f"{name}.c").write_text(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("assets", type=Path, help="Directory containing brain_<skin>.png files")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for skin in SKINS:
        convert(args.assets / f"brain_{skin}.png", args.output)


if __name__ == "__main__":
    main()

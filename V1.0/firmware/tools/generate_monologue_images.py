#!/usr/bin/env python3
"""Convert the App design library's four monologue PNGs to LVGL RGB565A8."""

from pathlib import Path

from PIL import Image

DESIGNS = (
    ("掉电中.png", "monologue_power"),
    ("心飞扬.png", "monologue_heart"),
    ("咖啡与我.png", "monologue_coffee"),
    ("信息流.png", "monologue_stream"),
)

SIZE = (152, 114)


def convert(source: Path, destination: Path, name: str) -> None:
    image = Image.open(source).convert("RGBA").resize(SIZE, Image.Resampling.LANCZOS)
    rgb565 = bytearray()
    alpha = bytearray()
    for red, green, blue, opacity in image.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        rgb565.extend((value & 0xFF, value >> 8))
        alpha.append(opacity)
    data = rgb565 + alpha
    lines = []
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
    output = f'''#include "brain_images.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

/* Generated from the App design library. RGB565 + 8-bit alpha. */
static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_data[] = {{
{chr(10).join(lines)}
}};

lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.w = {SIZE[0]},
    .header.h = {SIZE[1]},
    .header.stride = {SIZE[0] * 2},
    .data_size = sizeof({name}_data),
    .data = {name}_data,
}};
'''
    (destination / f"{name}.c").write_text(output)


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    source_dir = Path("/Users/tanshinan/Projects/feileme/V3.0 copy/design")
    output_dir = root / "src/images"
    output_dir.mkdir(parents=True, exist_ok=True)
    for filename, name in DESIGNS:
        convert(source_dir / filename, output_dir, name)
        print(f"wrote {name}.c")


if __name__ == "__main__":
    main()

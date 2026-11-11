#!/usr/bin/env python3
"""Re-sample the square RGB565A8 brain images down to the panel's draw size.

The assets were generated at 256x256, but BRAIN_SIZE is 236, so every frame was
scaled at draw time and 29.5 KB per image was spent on pixels that never reach
the panel. Storing them at 236 makes the draw 1:1 and frees ~236 KB of flash.

The script reads the generated C arrays (no source artwork needed) and rewrites
them in place at the new size.

Usage (from apps/brain_companion/):
    python3 tools/shrink_brain_images.py [size]
"""

import re
import sys
from pathlib import Path

from PIL import Image

# Only the square, fixed-size brains. The monologue cards keep their 4:3 frame.
TARGETS = (
    "brain_0", "brain_20", "brain_40", "brain_60", "brain_80", "brain_100",
    "brain_water", "brain_move",
)


def load(path: Path):
    text = path.read_text()
    width = int(re.search(r"\.header\.w = (\d+)", text).group(1))
    height = int(re.search(r"\.header\.h = (\d+)", text).group(1))
    body = re.search(r"_data\[\] = \{(.*?)\n\};", text, re.S).group(1)
    raw = bytes(int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]{2})", body))
    expected = width * height * 3
    if len(raw) != expected:
        raise ValueError(f"{path.name}: {len(raw)} bytes, expected {expected}")
    return width, height, raw


def to_image(width: int, height: int, raw: bytes) -> Image.Image:
    """RGB565 plane (little endian pairs) followed by an 8-bit alpha plane."""
    pixels = bytearray(width * height * 4)
    alpha_offset = width * height * 2
    for index in range(width * height):
        low = raw[index * 2]
        high = raw[index * 2 + 1]
        value = low | (high << 8)
        red = (value >> 11) & 0x1F
        green = (value >> 5) & 0x3F
        blue = value & 0x1F
        out = index * 4
        pixels[out] = (red << 3) | (red >> 2)
        pixels[out + 1] = (green << 2) | (green >> 4)
        pixels[out + 2] = (blue << 3) | (blue >> 2)
        pixels[out + 3] = raw[alpha_offset + index]
    return Image.frombytes("RGBA", (width, height), bytes(pixels))


def encode(image: Image.Image, size: int) -> bytes:
    rgb565 = bytearray()
    alpha = bytearray()
    for red, green, blue, opacity in image.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        rgb565.extend((value & 0xFF, value >> 8))
        alpha.append(opacity)
    return bytes(rgb565 + alpha)


def write(path: Path, name: str, data: bytes, size: int) -> None:
    lines = []
    for offset in range(0, len(data), 16):
        row = ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16])
        lines.append(f"    {row},")
    path.write_text(f'''#include "brain_images.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

/* Re-sampled to the panel draw size ({size}x{size}) from the 256px asset.
 * RGB565 pixels followed by an 8-bit alpha plane. */
static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_data[] = {{
{chr(10).join(lines)}
}};

lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.w = {size},
    .header.h = {size},
    .header.stride = {size * 2},
    .data_size = sizeof({name}_data),
    .data = {name}_data,
}};
''')


def main() -> int:
    size = int(sys.argv[1]) if len(sys.argv) > 1 else 236
    root = Path(__file__).resolve().parent.parent
    saved = 0
    for name in TARGETS:
        path = root / "src/images" / f"{name}.c"
        width, height, raw = load(path)
        if width == size and height == size:
            print(f"{name}: already {size}px")
            continue
        image = to_image(width, height, raw)
        resized = image.resize((size, size), Image.Resampling.LANCZOS)
        data = encode(resized, size)
        write(path, name, data, size)
        saved += len(raw) - len(data)
        print(f"{name}: {width}x{height} -> {size}x{size} "
              f"({len(raw)} -> {len(data)} bytes)")
    print(f"freed {saved} bytes ({saved // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

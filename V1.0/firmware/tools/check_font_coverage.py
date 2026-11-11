#!/usr/bin/env python3
"""Check that every glyph the UI asks for exists in the font it is drawn with.

LVGL renders a tofu box for any character absent from the font's cmap, and the
round panel is too small for that to be caught reliably by eye. Fonts here are
generated with a narrow, per-size charset: 18/30/44 only carry the handful of
fixed strings they draw, while 16/22 also carry App-synced content.

This walks each font's generated cmap table, resolves the strings passed to
label()/wrapped_label() for that font, and reports anything missing.

Usage (from apps/brain_companion/):
    python3 tools/check_font_coverage.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI_SOURCE = ROOT / "src/brain_ui.c"

# Content synced from the App at runtime cannot be harvested from the source,
# so the App's fixed copy is mirrored here for the two dynamic fonts.
DYNAMIC_TEXT = {
    "16": "夜猫子是我没错了小小糊涂虫念念不忘必有回响都是顺手的事不麻烦"
          "已读不回无情的计时器比例提高了继续加油哦"
          "抖音B站小红书视频号"
          "掉电中心飞扬咖啡与我信息流"
          "。「」！：；、～",
    "22": "夜猫子是我没错了小小糊涂虫念念不忘必有回响都是顺手的事不麻烦"
          "已读不回无情的计时器比例提高了继续加油哦"
          "抖音B站小红书视频号"
          "掉电中心飞扬咖啡与我信息流"
          "。「」！：；、～",
}

# Rendered through a variable rather than a literal argument. `text` on the
# realtime page is "脑力 %u" (digits only), `body` is App-synced insight copy.
VARIABLE_TEXT = {
    "text": "脑力0123456789",
    "body": "",
}


def font_codepoints(path: Path) -> set:
    text = path.read_text()
    covered = set()
    body = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if body is None:
        raise ValueError(f"{path}: no cmaps table")
    for entry in re.finditer(r"\{(.*?)\}", body.group(1), re.S):
        chunk = entry.group(1)
        start = int(re.search(r"\.range_start\s*=\s*(\d+)", chunk).group(1))
        length = int(re.search(r"\.range_length\s*=\s*(\d+)", chunk).group(1))
        unicode_list = re.search(r"\.unicode_list\s*=\s*(\w+)", chunk).group(1)
        if unicode_list == "NULL":
            covered.update(range(start, start + length))
            continue
        table = re.search(rf"{unicode_list}\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
        if table is None:
            raise ValueError(f"{path}: no {unicode_list} array")
        for token in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", table.group(1)):
            covered.add(start + int(token, 0))
    return covered


def string_arrays(text: str) -> dict:
    """`static const char *const NAME[] = { "a", "b", ... };` -> {NAME: [a, b]}"""
    arrays = {}
    for match in re.finditer(r"static const char \*const (\w+)\[\]\s*=\s*\{(.*?)\};",
                             text, re.S):
        arrays[match.group(1)] = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(2))
    return arrays


def call_sites(text: str, arrays: dict) -> dict:
    """Map font size -> set of characters drawn with that font."""
    fonts = {}
    for match in re.finditer(r"\b(label|wrapped_label)\s*\(([^;]*?)\)\s*;", text, re.S):
        args = match.group(2)
        font = re.search(r"&brain_font_(\d+)", args)
        if font is None:
            continue
        size = font.group(1)
        literal = re.search(r'^\s*[^,]+,\s*"((?:[^"\\]|\\.)*)"', args)
        if literal is not None:
            entry = literal.group(1)
        else:
            array_ref = re.search(r"^\s*[^,]+,\s*(\w+)\s*\[", args)
            if array_ref is not None and array_ref.group(1) in arrays:
                entry = "".join(arrays[array_ref.group(1)])
            else:
                call = re.search(r"^\s*[^,]+,\s*(\w+)\s*\(", args)
                if call is not None and call.group(1) == "current_tag":
                    entry = "".join(arrays.get("TAGS", []))
                else:
                    variable = re.search(r"^\s*[^,]+,\s*(\w+)\s*,", args)
                    key = variable.group(1) if variable is not None else None
                    entry = VARIABLE_TEXT.get(key, "")
        fonts.setdefault(size, set()).update(entry)
    return fonts


def main() -> int:
    text = UI_SOURCE.read_text()
    arrays = string_arrays(text)
    per_font = call_sites(text, arrays)
    for size, extra in DYNAMIC_TEXT.items():
        per_font.setdefault(size, set()).update(extra)

    missing_total = 0
    for size in sorted(per_font):
        font = ROOT / f"src/fonts/brain_font_{size}.c"
        if not font.exists():
            print(f"brain_font_{size}: source file missing")
            missing_total += 1
            continue
        covered = font_codepoints(font)
        used = {ord(ch) for ch in per_font[size] if ord(ch) > 0x7F}
        missing = sorted(ch for ch in used if ch not in covered)
        if missing:
            missing_total += len(missing)
            print(f"brain_font_{size}.c: missing {len(missing)} -> "
                  f"{''.join(chr(c) for c in missing)}")
        else:
            print(f"brain_font_{size}.c: complete ({len(used)} CJK glyphs used)")

    # 14/36 are excluded from the build; make sure nothing still points at them.
    for legacy in ("14", "36"):
        if re.search(rf"&brain_font_{legacy}\b", text):
            print(f"brain_font_{legacy}: still referenced by brain_ui.c "
                  f"but excluded in CMakeLists.txt")
            missing_total += 1
    return 1 if missing_total else 0


if __name__ == "__main__":
    sys.exit(main())

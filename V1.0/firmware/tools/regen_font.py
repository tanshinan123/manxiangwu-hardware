#!/usr/bin/env python3
"""Regenerate brain_font_16/22 with the extended charset.

The base charset covers every fixed UI string in brain_ui.c. Dynamic content
synced from the App (insight titles/bodies, postcard/monologue titles and
lines) needs extra glyphs, or LVGL renders tofu boxes on the panel.

Usage (from apps/brain_companion/):
    python3 tools/regen_font.py
"""

import re
import subprocess
from pathlib import Path

# Original symbol list from the 22px font header (fixed UI strings).
BASE_SYMBOLS = (
    "一上下中习事件会伴你倦做停健儿先再出切刚到前力动取受口右后吗吧呢哭啦喝嘶"
    "回圈在好子学它安完定小就屋左已康形态恋情想慢成我才按换掉接提收新时暂有束"
    "来柔查段水没涂消滑演点爱状独留疲白的直看眼睡确示秒穿站糊累结续美脑脱色装"
    "要觉解话详诶请读象走起身软轻运返还这退选酷里锁间阅陪难黑，？～"
)

# Dynamic content synced over CONTENT_SYNC (InsightEngine titles + postcard
# pool titles/lines + app display names + safety punctuation).
CONTENT_SYMBOLS = (
    "夜猫子是我没错了小小糊涂虫念念不忘必有回响都是顺手的事不麻烦"
    "已读不回无情的计时器比例提高了继加油哦哦计器无"
    "抖音B站小红书视频号"
    # 设计库四张独白图的标题/正文（掉电中 / 心飞扬 / 咖啡与我 / 信息流）
    "掉电中i人持续充电宝呢心飞扬是谁吸引了注意力呀咖啡与我续命ing、工作和"
    "随时冲散在信息流法自拔"
    "。「」！：；、"
)

# New fixed UI strings introduced by the collection-page refresh.
UI_SYMBOLS = "测试流程皮肤成就我陪你看看其他事情吧"


def current_16_symbols(root: Path) -> str:
    """Keep every glyph the existing 16px font already ships."""
    source = root / "src/fonts/brain_font_16.c"
    match = re.search(r"--symbols (.*?) --size", source.read_text(), re.S)
    return match.group(1) if match else ""


def build_symbols(extra: str) -> str:
    symbols = "".join(dict.fromkeys(BASE_SYMBOLS + CONTENT_SYMBOLS + UI_SYMBOLS + extra))
    return "".join(ch for ch in symbols if not (" " <= ch <= "~"))


def generate(root: Path, size: int, symbols: str) -> bool:
    cmd = [
        "npx", "--yes", "lv_font_conv@1.5.3",
        "--font", str(root / "assets/brain_ui_font.ttf"),
        "-r", "0x20-0x7F",
        "--symbols", symbols,
        "--size", str(size), "--bpp", "4",
        "--format", "lvgl", "--no-compress",
        "--lv-font-name", f"brain_font_{size}",
        "-o", str(root / f"src/fonts/brain_font_{size}.c"),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        return False
    out = root / f"src/fonts/brain_font_{size}.c"
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    return True


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    symbols_22 = build_symbols("")
    symbols_16 = build_symbols(current_16_symbols(root))
    print(f"22px charset: {len(symbols_22)} glyphs + ASCII")
    print(f"16px charset: {len(symbols_16)} glyphs + ASCII")
    if not generate(root, 22, symbols_22):
        return 1
    return 0 if generate(root, 16, symbols_16) else 1


if __name__ == "__main__":
    raise SystemExit(main())

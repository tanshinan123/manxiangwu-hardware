#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
日志监控官 —— 常驻监听脑搭子串口，实时过滤 BLE 连接/断联/订阅/写帧/ACK/崩溃事件。

用途：排查「连不上 / 反复断连 / 数据没同步」这类问题。手动快照式抓日志很容易
漏掉连接那一两秒的关键事件，本工具持续盯着串口，把关键事件按时间轴实时打印，
并统计崩溃与断连次数。

用法：
    python3 log_monitor.py                     # 默认 /dev/cu.usbmodem* 第二个口，460800
    python3 log_monitor.py --port /dev/cu.usbmodem5B220305453
    python3 log_monitor.py --duration 120      # 只监听 120 秒后退出
    python3 log_monitor.py --all               # 打印全部原始行（不过滤）

依赖：pyserial（PYTHONPATH=/tmp/pylibs 或 pip install pyserial）
"""
import argparse
import re
import sys
import time
from datetime import datetime

try:
    import serial
except ImportError:
    print("缺少 pyserial：pip install pyserial，或 PYTHONPATH=/tmp/pylibs", file=sys.stderr)
    sys.exit(2)


# 关键事件分类。顺序即优先级，一行命中多个时取最靠前的类别。
PATTERNS = [
    ("CRASH",      re.compile(r"AP crash|Usage fault|Hard fault|BusFault|Assert|assert failed|rst:")),
    ("MEM",        re.compile(r"ERR IN MEM|mem_alloc|out of mem|OOM|os_memblock")),
    ("CONNECT",    re.compile(r"connected|connect|GAP_EVENT_CONNECT|link up")),
    ("DISCONNECT", re.compile(r"disconnect|GAP_EVENT_DISCONNECT|link down|link lost")),
    ("SUBSCRIBE",  re.compile(r"subscribe|notify=|ACK notify|SUBSCRBE")),
    ("ADV",        re.compile(r"advertis|adv_active|adv start|broadcast")),
    ("WRITE",      re.compile(r"write|WRITE_CHR|Rx Op|WRITE_REQ|ACCESS_OP_WRITE")),
    ("FRAME",      re.compile(r"\[brain_ble\]|\[brain\]|STATE_|APP_LAUNCH|FOCUS_|BREAK_|CONTENT_|REFLECT_|SKIN_")),
    ("ACK",        re.compile(r"ACK|ack")),
    ("VOICE",      re.compile(r"voice|audio|tdl_audio|play")),
    ("UI",         re.compile(r"brain-ui|brain_ui|lv_")),
]


def classify(line: str):
    for name, pat in PATTERNS:
        if pat.search(line):
            return name
    return None


def main():
    ap = argparse.ArgumentParser(description="脑搭子日志监控官")
    ap.add_argument("--port", default=None, help="串口设备，默认自动探测 usbmodem 第二个口")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--duration", type=float, default=0, help="监听秒数，0=常驻")
    ap.add_argument("--all", action="store_true", help="打印全部原始行")
    args = ap.parse_args()

    port = args.port
    if port is None:
        import glob
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if not ports:
            print("未找到 /dev/cu.usbmodem*，请接上硬件或手动 --port", file=sys.stderr)
            sys.exit(2)
        # 烧录口通常是 …451，日志口是 …453，取最后一个。
        port = ports[-1]

    print(f"== 日志监控官：{port} @ {args.baud} ==", flush=True)
    try:
        ser = serial.Serial(port, args.baud, timeout=0.5)
    except Exception as e:
        print(f"打开串口失败：{e}", file=sys.stderr)
        sys.exit(2)

    counts = {}
    start = time.time()
    buf = b""
    try:
        while True:
            chunk = ser.read(8192)
            if chunk:
                buf += chunk
                while b"\n" in buf or b"\r" in buf:
                    # 按 \r\n 或 \n 拆行
                    nl = buf.find(b"\n")
                    cr = buf.find(b"\r")
                    if nl == -1 and cr == -1:
                        break
                    cut = min(x for x in (nl, cr) if x != -1)
                    line = buf[:cut].decode("utf-8", "replace").strip()
                    buf = buf[cut + 1:]
                    if not line:
                        continue
                    ts = datetime.now().strftime("%H:%M:%S")
                    cat = classify(line)
                    if args.all or cat:
                        counts[cat] = counts.get(cat, 0) + 1
                        tag = cat or "···"
                        print(f"[{ts}][{tag}] {line}", flush=True)
            if args.duration and time.time() - start >= args.duration:
                break
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("\n== 汇总 ==", flush=True)
        for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
            print(f"  {k}: {v}", flush=True)


if __name__ == "__main__":
    main()

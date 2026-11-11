# 慢想屋 · 电子吧唧 V1.0

桌面 / 手机背部的陪伴终端。手机通过本地 BLE 把「脑力」「陪伴倒计时」「收藏内容」同步到板子上，
板子把用户的选择（提前结束、一件小事、换皮肤）回传给手机。**不经过云端**，
搜索词、意图原文、行为明细只留在手机端。

- 硬件：微雪 `T5-E1-Touch-AMOLED-1.75`（466 × 466 圆屏，T5AI，电容触摸，BLE 5.4）
- 固件基线：TuyaOpen **v1.6.0**（`tos.py`）
- UI：LVGL 9
- 手机端：慢想屋 V3.0（iOS 17+）

## 目录

```
V1.0/
├── firmware/            TuyaOpen 应用，整个目录丢进 TuyaOpen/apps/ 即可
│   ├── src/
│   │   ├── app_main.c       入口、线程、BLE ↔ UI 胶水（含显示锁）
│   │   ├── brain_ble.c/.h   GATT 服务 + JSON 协议编解码 + ACK
│   │   ├── brain_ui.c/.h    LVGL 全部页面、状态机、本地计时
│   │   ├── brain_voice.c/.h 音频线程与 PCM 分帧推流
│   │   ├── brain_images.h   20 张图的声明
│   │   ├── voice_clips.c    两段 16 kHz PCM（生成代码）
│   │   ├── images/          16 张脑子图 + 4 张独白图（生成代码）
│   │   └── fonts/           16/18/22/30/44 五档中文字库（生成代码；14/36 已被 CMake 排除）
│   ├── tools/          协议探针 + 图片/字体/语音生成脚本
│   ├── config/         板级配置
│   ├── assets/         字体 TTF、两段 WAV 源
│   └── docs/           firmware-guide.md（完整固件说明书）
└── README.md
```

手机侧的实现不在本仓库，这里只定义板子这一端的行为与协议。
**逐条细节请看 [`docs/firmware-guide.md`](docs/firmware-guide.md)**，本文只做概览。

## 两条主流程

**第一主流程：实时脑力 → 专注陪伴 → 到点/提前结束 → 一件小事**

- 实时脑力页：六档脑子、脑力值、状态文案，2.8 s 呼吸
- 陪伴倒计时页：442 圆环消减、`MM:SS`、脑子呼吸；硬件本地计时，不依赖手机前台
- 最后 5 秒：呼吸加快加深，整屏背景随呼吸起伏（呼吸灯，不用红色、不硬闪），并播语音
- 到点：播「我陪你看看其他事情吧～」→ 直接进「一件小事」
- 提前结束：长按 2 s 直接结束并进「一件小事」，**没有二次确认页**
- 一件小事：一屏一项左右滑（喝水/走动/睡觉/回到刚才的事），确认后展示 15 s 自动回实时页

**第二主流程：皮肤 / 独白 / 成就 + 演示 / 测试**

- 实时页 5 个入口默认是 48×48 图标，轻点屏幕空白处展开成文字按钮，静止 10 s 自动收回
- 皮肤：8 款全部展示，未解锁的右上角显示 ✕ 且没有操作钮；单穿语义，穿新款自动换下旧款；断电重启保留
- 独白：design 库 4 张固定图（掉电中 / 心飞扬 / 咖啡与我 / 信息流），展示图 + 标题，解锁后可阅读
- 成就：淡黄便签，标题 + 正文（正文来自 App 同步）
- 演示：轻点脑子循环 6 档，不写 KV、不发事件
- 测试流程：直接以 30 s 起一次专注，`ui.testing` 拦掉所有上行事件，手机端看不到这轮

## 协议（BLE GATT）

服务 `12345678-1234-5678-9ABC-DEF012345601`，广播名 `SlowHouse-Brain`。

| 特征 | UUID 尾号 | 方向 | 说明 |
| --- | --- | --- | --- |
| 写入 | `…602` | 手机 → 板子 | JSON 帧；也接受 1 字节脑力值（调试用） |
| 通知 | `…603` | 板子 → 手机 | JSON 事件；1 字节写入时回 2 字节 ACK |

帧格式（UTF-8 JSON）：

```json
{"protocolVersion":1,"eventID":"dev-00008d9a","sessionID":"","eventType":"STATE_SYNC","timestamp":1762410000,"payload":{}}
```

| 事件 | 方向 | 关键 payload |
| --- | --- | --- |
| `APP_LAUNCH` / `STATE_SYNC` | 手机→板子 | `brainScore`, `unlockedSkinMask`, `wearingSkin`, `durationSeconds` |
| `REFLECT_DONE` | 手机→板子 | `brainScore`；结束倒计时但**不动**正在显示的小事页 |
| `FOCUS_START` | 手机→板子 | `durationSeconds`（**必须**）、`brainScore` |
| `FOCUS_END` | 手机→板子 | `brainScore`, `reason` |
| `BREAK_MENU` / `BREAK_ACTION` | 手机→板子 | `breakType`: `water`/`move`/`sleep`/`return` |
| `CONTENT_SYNC` | 手机→板子 | `contentKind`: `postcard`/`insight`；`index`, `present`, `title`, `text`（独白允许 `img`，固件已忽略）；不逐帧回 ACK |
| `STATE_REQUEST` | 板子→手机 | 订阅成功后立即发，手机用 `STATE_SYNC` 回答 |
| `STATE_ACK` | 板子→手机 | `ackEventID`, `status`, `brainScore`（设备真值）, `capabilities` |
| `END_REQUEST` | 板子→手机 | 用户长按「提前结束」满 2 s |
| `TIME_UP` | 板子→手机 | 倒计时到点 |
| `BREAK_ACTION` | 板子→手机 | `detail` = 用户选的小事 |
| `SKIN_SET_REQUEST` | 板子→手机 | `detail`: 皮肤名，或空串表示脱下 |

`STATE_ACK.capabilities = ["CONTENT_SYNC","SKIN_SET"]`。App 在收到能力声明前不发内容类帧，
因此新版 App 连老固件也安全。`status` 取值：`0` 成功、`1` 长度非法、`2` 脑力值非法。

`unlockedSkinMask` bit 0–7 顺序必须与 App 的 `Skin.allCases` 完全一致：
`sleep, love, dizzy, hurt, cool, study, health, sport`。
穿过皮肤在本地 KV `brain_skin` 持久化（`0xFF` = 未穿）；App 重连后以 App 的权威状态校正。

**并发约束**：固件接收缓冲 384 B，上行 320 B；App 侧单帧控制在 250 B 以内并逐字裁剪，
且必须单条在途写入 —— 连续多条 `withResponse` 会打爆 NimBLE 动态 mbuf 池（详见说明书 §8.2）。

**幂等**：板子用 6 槽环形记录最近处理过的 `eventID`，重复帧只回 ACK 不改界面。

## 构建与烧录

把 `firmware/` 放进 TuyaOpen 的应用目录，例如 `TuyaOpen/apps/brain_companion/`，然后：

```sh
cd TuyaOpen && source ./export.sh
cd apps/brain_companion
../../.venv/bin/python ../../tos.py build
```

烧录（`tos.py flash` 若因缺 `click` 失败，用 tyutool 直调）：

```sh
.venv-flash/bin/python tools/tyutool/tyutool_cli.py write -d T5AI \
  -f apps/brain_companion/.build/bin/brain_companion_QIO_1.0.0.bin \
  -p /dev/cu.usbmodemXXXX -b 2000000
```

产物在 `.build/bin/`（同步一份到 `dist/brain_companion_1.0.0/`）。
当前烧录版 `brain_companion_QIO_1.0.0.bin` 为 **5,208,448 字节**，SHA256：
`54085ba0f98b7788a7a025cb4ec36331ea9e0c34a0f1b2bb937ad854067e0c46`。

**注意**

- `source export.sh` 会把工作目录切回 TuyaOpen 根目录，之后必须自己 `cd` 回应用目录，
  否则会报 `TuyaOpen root cannot be regarded as project root`
- 构建进程必须能自由删除文件，受限沙箱里 ninja 会报 `failed recompaction: Operation not permitted`
- 当前采用**原地迭代**：备份旧 QIO 后用同名 QIO 覆盖，不擦整片 Flash、不动 NVS/KV

## 已知限制

- **Flash 已到 97.89%（3,656,804 / 3,648 KB）**，只剩约 78 KB。再加素材或扩字会链接失败，
  详见说明书 §8.1
- 皮肤形象按 160×160 存储、显示时放大到 236（8 张都拉到 236 还需约 380 KB，放不下）
- **设备没有校时**：无网时 `tal_time_get_posix()` 返回开机秒数，`timestamp` 不可信；
  倒计时一律以手机下发的 `durationSeconds` 为准
- 成就最多 2 条，与 App 的 `InsightEngine.maxShown` 对齐
- 小事页只做了滑动选择，没有轮播指示点
- 板子同时只能被一个中心设备连接：测试时先断开 nRF Connect，否则手机连不上

## 测试与生成工具

`firmware/tools/` 下的 macOS 端探针（CoreBluetooth，Swift 脚本，直接 `swift xxx.swift` 运行）：

- `ble_probe.swift`：六档脑力写入 + 非法值校验，验证 GATT 与 ACK
- `proto_probe.swift`：协议回归（`STATE_REQUEST` / `STATE_SYNC` / `FOCUS_START` / 重复帧 / `BREAK_MENU`）
- `full_flow_probe.swift`：两条主流程一起跑

资源生成（改了文案或素材后必须重跑）：

| 脚本 | 作用 |
| --- | --- |
| `regen_font.py` | 重建 16/22 号字库 |
| `check_font_coverage.py` | 按字号核对字库覆盖，缺字直接报字 |
| `generate_skin_images.py` | 皮肤图 → LVGL RGB565A8 |
| `generate_monologue_images.py` | design 库 4 张独白图 → LVGL |
| `shrink_brain_images.py` | 脑子图重采样到显示尺寸（省 flash） |
| `generate_voice.py` | WAV → `voice_clips.c`（换 WAV 后必须重跑） |

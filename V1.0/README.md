# 慢想屋 · 电子吧唧 V1.0

桌面 / 手机背部的陪伴终端。手机通过本地 BLE 把「脑力」「陪伴倒计时」同步到板子上，
板子把用户的选择（提前结束、续时、按时、小事）回传给手机。**不经过云端**，
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
│   │   ├── app_main.c       入口与 BLE ↔ UI 胶水
│   │   ├── brain_ble.c/.h   GATT 服务 + JSON 协议编解码
│   │   ├── brain_ui.c/.h    LVGL 界面与本地计时
│   │   ├── brain_images.h
│   │   ├── images/          9 张 256×256 RGB565A8 脑子图（生成代码）
│   │   └── fonts/           7 档中文字库（生成代码）
│   ├── tools/          自动化测试探针
│   ├── config/         板级配置
│   └── assets/preview/ 界面预览图
└── README.md
```

手机侧的实现不在本仓库，这里只定义板子这一端的行为与协议。

## 已实现

**界面**
- 实时脑力页：六档脑子、脑力值、状态文案，2.8 s 呼吸；右上「演示」进出六态演示
- 陪伴倒计时页：薄荷绿外环、`MM:SS`、脑子呼吸
- 最后 5 秒：呼吸变快变深，背景薄荷随呼吸起伏（呼吸灯，不用红色、不硬闪）
- 提前结束：长按 2 s → 「取消 / 结束」，取消回到原剩余秒数
- 到点决策：首次 →「续时5min / 按时」；续时后再到点 → 只剩「确定」
- 一件小事：一屏一项，左右滑动，「就选它」确认；结果展示 15 s 自动回实时页
- 背景：竖向薄荷渐变，参考 App 的 `mist → surface`

**协议**（BLE GATT，服务 `12345678-1234-5678-9ABC-DEF012345601`）

| 特征 | UUID 尾号 | 方向 | 说明 |
| --- | --- | --- | --- |
| 写入 | `…602` | 手机 → 板子 | JSON 帧；也接受 1 字节脑力值（调试用） |
| 通知 | `…603` | 板子 → 手机 | JSON 事件；1 字节写入时回 2 字节 ACK |

帧格式（UTF-8 JSON）：

```json
{"protocolVersion":1,"eventID":"uuid","sessionID":"uuid","eventType":"STATE_SYNC","timestamp":1762410000,"payload":{}}
```

| 事件 | 方向 | 关键 payload |
| --- | --- | --- |
| `APP_LAUNCH` / `STATE_SYNC` / `REFLECT_DONE` | 手机→板子 | `brainScore`, `brainAsset`, `statusText` |
| `FOCUS_START` / `FOCUS_EXTEND` | 手机→板子 | `durationSeconds`（**必须**）, `endsAt`, `brainScore` |
| `FOCUS_END` | 手机→板子 | `brainScore`, `reason` |
| `BREAK_MENU` / `BREAK_ACTION` | 手机→板子 | `breakType`: `water`/`move`/`sleep`/`return` |
| `STATE_REQUEST` | 板子→手机 | 订阅成功后立即发，手机用 `STATE_SYNC` 回答 |
| `STATE_ACK` | 板子→手机 | `ackEventID`, `status`, `brainScore`（设备真值） |
| `END_REQUEST` | 板子→手机 | 用户在板子上确认了提前结束 |
| `TIME_UP` | 板子→手机 | 倒计时到点 |
| `EXTEND_REQUEST` | 板子→手机 | 用户点了「续时」 |
| `KEEP_REQUEST` | 板子→手机 | 用户点了「按时」或「确定」 |

幂等：板子用 6 槽环形记录最近处理过的 `eventID`，重复帧只回 ACK 不改界面；
`FOCUS_START` 之外的事件带旧 `sessionID` 会被忽略。

## 构建与烧录

把 `firmware/` 放进 TuyaOpen 的应用目录，例如 `TuyaOpen/apps/brain_companion/`，然后：

```sh
cd TuyaOpen && source ./export.sh
cd apps/brain_companion
tos.py build
tos.py flash -p /dev/cu.usbmodemXXXX
```

产物在 `dist/brain_companion_1.0.0/`，QIO 约 4.7 MB（分区上限 6.7 MB）。

**注意**：`source export.sh` 会把工作目录切回 TuyaOpen 根目录，之后必须自己 `cd` 回应用目录，
否则会报 `TuyaOpen root cannot be regarded as project root`。

烧录依赖 `rich` 与 `pyserial`，SDK 的 `requirements.txt` 里没有，需自行安装。

## 已知限制

- **语音未实现**：到点应播「刷好了吗？」，当前只有界面提示
- **设备没有校时**：`tal_time_get_posix()` 在无网时返回开机秒数，所以 `timestamp` 字段不可信；
  倒计时一律以手机下发的 `durationSeconds` 为准
- 字体源文件未随仓库提供：`src/fonts/*.c` 是生成好的字库，可直接编译；
  要改文案需用同一套字模重新生成
- 小事页只做了滑动选择，没有做轮播指示点
- 板子同时只能被一个中心设备连接：测试时先断开 nRF Connect，否则手机连不上

## 测试工具

`firmware/tools/` 下两个 macOS 端探针（CoreBluetooth，Swift 脚本，直接 `swift xxx.swift` 运行）：

- `ble_probe.swift`：六档脑力写入 + 非法值校验，验证 GATT 与 ACK
- `proto_probe.swift`：完整协议回归（`STATE_REQUEST` / `STATE_SYNC` / `FOCUS_START` / 重复帧 / `BREAK_MENU`），
  加 `--extend` 可验证「续时后再到点」这条分支

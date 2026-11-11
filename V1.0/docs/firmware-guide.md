# 慢想屋 · 脑搭子 —— 固件说明书（当前烧录版）

> 本文对应**实际烧录到板子上的那一版代码**，覆盖第一主流程（实时脑力 → 专注陪伴 → 到点/提前结束 → 一件小事）与第二主流程（皮肤 / 独白 / 成就 + 演示 / 测试），并给出可复制的编译、烧录、验证步骤。

| 项目 | 值 |
|---|---|
| 文档日期 | 2026-11-11 |
| 固件版本 | `brain_companion 1.0.0` |
| 源码位置 | `TuyaOpen-160/apps/brain_companion/` |
| 烧录产物 | `brain_companion_QIO_1.0.0.bin`，**5,208,448 字节** |
| SHA256 | `54085ba0f98b7788a7a025cb4ec36331ea9e0c34a0f1b2bb937ad854067e0c46` |
| 备份副本 | `~/Projects/brain_companion_v20_collections_ui.bin` |
| 烧录记录 | Write flash success → CRC check success → Reboot done |

> ⚠️ 本仓库 `V1.0/firmware/` 是历史快照镜像，界面与协议逐条细节**以 TuyaOpen 工程内源码为准**。本文描述的是后者。

---

## 1. 硬件与工程基线

| 项目 | 规格 |
|---|---|
| 主控 | 涂鸦 T5AI（BK7258 双核：AP + CP，Cortex-M33） |
| 板卡 | 微雪 `WAVESHARE_T5AI_TOUCH_AMOLED_1_75` |
| 屏幕 | 466 × 466 圆形 AMOLED，电容触摸 |
| 屏亮度 | 固件固定 80（`BRAIN_BRIGHTNESS`） |
| 无线 | BLE 5.4（Tuya NimBLE），**仅本地链路，不连云** |
| 图形 | LVGL 9，渲染线程 8 KB 栈 |
| SDK | TuyaOpen **v1.6.0**（`tos.py`） |
| 工程配置 | `CONFIG_BOARD_CHOICE_WAVESHARE_T5AI_TOUCH_AMOLED_1_75=y`、`CONFIG_ENABLE_LIBLVGL=y`、`CONFIG_LVGL_ENABLE_TP=y` |

### 1.1 分区表（烧录时打印）

```
primary_bootloader   0x00000000     68K
primary_cp_app       0x00011000   1224K
primary_ap_app       0x00143000   3876K   ← 应用镜像落在这里
ota                  0x0050c000   1780K
ota_mgr              0x006c9000      8K
usr_config           0x006cb000   1024K
res                  0x007cb000      8K
tuya_data            0x007cd000    196K   ← KV（皮肤持久化）
sys_rf               0x007fe000      4K
sys_net              0x007ff000      4K
```

### 1.2 当前占用（关键约束）

| 区域 | 已用 | 容量 | 占比 |
|---|---|---|---|
| FLASH（AP 应用） | 3,656,804 B | 3,648 KB | **97.89%** |
| RAM | 72,916 B | 344 KB | 20.70% |
| DTCM | 13,084 B | 16,380 B | 79.88% |

**Flash 只剩约 78 KB**。这一版是靠把脑子图从 256² 降到 236²（省 236 KB）才挤进去的。再加素材或扩字会直接链接失败 —— 详见 §8.1。

---

## 2. 目录结构与模块职责

```
apps/brain_companion/
├── CMakeLists.txt          源文件收集；排除未使用的 14/36 号字库
├── app_default.config      板级开关
├── src/
│   ├── app_main.c    (221) 入口、线程、BLE ↔ UI 胶水、显示锁
│   ├── brain_ble.c   (612) GATT 服务 + JSON 协议编解码 + ACK
│   ├── brain_ble.h    (52) 协议帧结构体、回调类型
│   ├── brain_ui.c   (1091) 全部页面、状态机、本地计时、LVGL 布局
│   ├── brain_ui.h     (64) 页面 API 与对外事件
│   ├── brain_voice.c  (95) 音频线程、PCM 分帧推流
│   ├── brain_images.h (27) 20 张图的 LVGL 声明
│   ├── voice_clips.c(12726) 两段 16 kHz PCM（生成代码）
│   ├── images/            16 张脑子图 + 4 张独白图（生成代码）
│   └── fonts/             16/18/22/30/44 五档中文字库（生成代码）
├── assets/                字体 TTF、两段 WAV 源
└── tools/                 资源生成 + 验证探针
```

### 2.1 运行期线程

| 线程 | 栈 | 优先级 | 职责 |
|---|---|---|---|
| `brain_companion`（app 主线程） | 4 KB | 4 | `user_main`：初始化 KV/音频/显示/UI/BLE 后自删 |
| LVGL vendor 渲染线程 | 8 KB | `THREAD_PRIO_0` | LVGL 定时器与刷新 |
| `brain_voice` | 4 KB | 2 | 独占 codec，按帧推 PCM，不阻塞 UI |
| NimBLE host（SDK） | SDK 配置 | — | GATT 事件、帧解析 |

**跨线程铁律**：BLE 回调全部跑在 NimBLE host task 上，任何 LVGL 调用都必须先 `lv_vendor_disp_lock()` / `lv_vendor_disp_unlock()`（见 `app_main.c` 的 `on_ble_score` / `on_ble_frame`）。漏了会让渲染线程走到半成品对象树。

---

## 3. 第一主流程：实时脑力 → 专注陪伴 → 到点/提前结束 → 一件小事

### 3.1 端到端时序

```
App                          BLE                  板子
 │                            │                    │
 ├─ APP_LAUNCH / STATE_SYNC ─►│───────────────────►│ 实时页：脑力档位 + 形象 + 文案
 │                            │◄──── STATE_ACK ────┤ status/brainScore/capabilities
 │                            │                    │
 ├─ FOCUS_START(seconds) ────►│───────────────────►│ 专注页：圆环倒计时 + 呼吸
 │                            │                    │ 本地计时，不依赖手机前台
 │                            │                    │ 剩 5 s：语音 + 加快呼吸/背景脉动
 │                            │◄──── TIME_UP ──────┤ 到点：语音「我陪你看看其他事情吧～」
 │                            │                    │ 直接进「一件小事」选页
 ├─ BREAK_MENU（可选） ──────►│───────────────────►│ 也可由手机直接推到选页
 │                            │◄── BREAK_ACTION ───┤ 用户选中：water/move/sleep/return
 ├─ BREAK_ACTION ────────────►│───────────────────►│ 15 s 小事页，自动回实时页
 │                            │                    │
 │                    长按「提前结束」2 s          │
 │                            │◄── END_REQUEST ───┤ 直接进「一件小事」选页（无确认页）
 ├─ FOCUS_END(score) ────────►│───────────────────►│ 实时页 + 最新脑力
```

### 3.2 页面与状态机

固件内 `screen_state_t` 共 9 态，第一主流程用前 5 个：

| 状态 | 页面 | 进入方式 | 退出方式 |
|---|---|---|---|
| `SCREEN_REALTIME` | 实时脑力页 | 上电 / 结束 / 各页返回 | 点入口按钮 |
| `SCREEN_BRAIN_DEMO` | 脑力演示页 | 左下「演示」 | 「退出」返回 |
| `SCREEN_FOCUS` | 专注陪伴页 | `FOCUS_START` / 长按态恢复 / `STATE_SYNC(seconds>0)` | 到点、提前结束、`FOCUS_END` |
| `SCREEN_BREAK_MENU` | 一件小事选页 | 到点 / 提前结束 / `BREAK_MENU` | 选中一项 / 无操作停留 |
| `SCREEN_BREAK` | 小事进行页 | 选中后 | 15 s 自动回实时页 |

**关键决策：提前结束没有二次确认页。** 2 秒长按本身就是防误触，长按满即发 `END_REQUEST` 并直接落到「一件小事」。上一版的「先停一下 / 取消 / 结束」确认页已整页删除（`SCREEN_EARLY_CONFIRM` 从枚举中移除）。

### 3.3 页面布局（466 圆屏）

**实时脑力页**

| 元素 | 内容 | 字号 | 位置 |
|---|---|---|---|
| 标题 | 慢想屋 | 22 | 顶部居中 y=30 |
| 脑力值 | 脑力 80 | 44 | y=60 |
| 脑子 | 当前形象或状态图，236² | — | y=126 |
| 状态胶囊 | 300×52 圆角 26 | 30 | y=368 |
| 提示 | 我在这儿，慢慢陪你 | 16 灰 | y=426 |

**专注陪伴页**

| 元素 | 参数 |
|---|---|
| 外环 | 442² 圆环，线宽 9，逆时针消减 |
| 标题 | 陪伴中，22 号，y=30 |
| 脑子 | 236²，y=64，2.8 s 呼吸（缩放 ±12/256） |
| 倒计时 | 44 号等宽感（字距 3），y=312 |
| 进度条 | 174×6，y=360（长按进度，放在按钮上方避免手指遮挡） |
| 提前结束 | 174×52 胶囊，y=374，长按 2 s |

**最后 5 秒**：`remaining <= 5` 时呼吸周期从 2800 ms 缩到 2000 ms、振幅加大，同时整屏背景在 `BG_TOP↔BG_PULSE` 之间做呼吸脉动、圆环透明度同步起伏 —— 是「呼吸灯」不是闪烁，全程无红色告警。

**一件小事**

| 元素 | 参数 |
|---|---|
| 标题 | 陪你做的一件小事，22 号，y=24 |
| 翻页区 | 466×300，y=62，一屏一项，左右滑 |
| 每项 | 脑子 236² @0 + 文案 22 号 @250 |
| 确认 | 右下角 OK 圆钮 52，贴圆边 |
| 提示 | 左右滑动，选一件，16 灰 @386 |

四项固定映射：`water` 起身喝口水吧～ / `move` 站起来走一走呢～ / `sleep` 美美睡一觉吧～ / `return` 回到刚才的事吧～（`return` 显示当前脑子图）。选中后进 15 s 小事页，到时自动回实时页。

### 3.4 固件本地能力（不依赖手机）

| 能力 | 实现 |
|---|---|
| 本地计时 | `lv_tick_get()` 差值，100 ms 定时器刷新，不依赖 RTC 与手机 |
| 到点判定 | `remaining == 0` → 播语音 → `TIME_UP` → 进小事页 |
| 5 秒提醒 | `remaining == 5` → 播 `five.wav` |
| 语音播放 | 独立线程按 `frame_size` 分帧 `tdl_audio_play`，codec 常开不复开（T5AI 二次 open 会 `aud_adc_init` 失败并 fault） |
| 呼吸动画 | 三角波 `breath_level()`，所有含脑子页面共用 |

### 3.5 事件映射表（第一主流程）

| 方向 | 事件 | payload | 固件行为 |
|---|---|---|---|
| App→设备 | `APP_LAUNCH` / `STATE_SYNC` | `brainScore`、`unlockedSkins`、`wearingSkin`、`durationSeconds` | 刷形象/脑力；`seconds>0` 则续专注页，否则若在会话页则回实时页 |
| App→设备 | `FOCUS_START` | `seconds` | 进专注页并起本地倒计时 |
| App→设备 | `FOCUS_END` | `brainScore` | 回实时页并刷新脑力（缺省 80） |
| App→设备 | `REFLECT_DONE` | `brainScore` | 同上，但**不动**正在显示的小事页（让 15 s 跑完） |
| App→设备 | `BREAK_MENU` | — | 直接进小事选页 |
| App→设备 | `BREAK_ACTION` | `breakType` | 直接进对应小事页 |
| 设备→App | `TIME_UP` | `{}` | 到点，App 结算 |
| 设备→App | `END_REQUEST` | `{}` | 用户长按提前结束 |
| 设备→App | `BREAK_ACTION` | `detail: water/move/sleep/return` | 用户在板子上选了小事 |
| 设备→App | `STATE_REQUEST` | `{}` | 连上后主动要快照 |

---

## 4. 第二主流程：皮肤 / 独白 / 成就（收藏与本地互动）

### 4.1 入口与显隐交互

实时页有 **5 个入口**，贴圆边分布：

| 索引 | 文字 | 图标态符号 | 位置（左上/右上锚点） |
|---|---|---|---|
| 0 | 演示 | ▶ | 左 (16, 258) |
| 1 | 测试流程 | ☰ | 左 (44, 316) |
| 2 | 皮肤 | 🖼 | 右 (-50, 92) |
| 3 | 独白 | 📄 | 右 (-18, 150) |
| 4 | 成就 | 🔋 | 右 (-4, 208) |

- **默认只显示 48×48 圆形图标**，页面保持安静；
- **轻点屏幕任意空白处** → 5 个按钮同时展开成 88×46 文字按钮；
- **10 秒无操作**自动收回图标态（`TOOL_TEXT_MS = 10000`），再点一下也能手动收回。
- 按钮点击不会冒泡到根节点，所以点按钮不会误触发切换。

> 三个收藏入口沿圆弧阶梯式贴边（每个都在该行内接圆内约 2 px），不是一条直线，避免圆屏四角被切。

### 4.2 三个收藏页

**皮肤（8 款全展示）**

| 元素 | 说明 |
|---|---|
| 标题 | 皮肤，22 号，y=30；右上角 44 圆形关闭钮 |
| 翻页 | 一屏一款，左右滑，共 8 款（安睡/恋爱/糊涂/难受/酷酷/学习/健康/运动） |
| 形象 | 236² 居中 @0 |
| 名称 | 22 号 @244 |
| 解锁徽标 | 36² 圆钮贴右上（-18, 236）：✔ = 已解锁，✕ = 未解锁 |
| 操作钮 | 174×50 @382，**仅解锁款显示**；未穿 = 「穿上」，已穿 = 「脱下」 |
| 提示 | 左右滑动，16 灰 @438 |

- 下方不再有「已解锁/已穿上」文字行（按要求移除，状态只靠徽标 + 按钮文案）。
- **单穿语义**：全局只有一个 `ui.wearing`，穿上新款自动顶替旧款，不存在多件叠加。
- 脱下后实时页回落到六档状态图。

**独白（4 张设计库图）**

| 元素 | 说明 |
|---|---|
| 标题 | 独白，22 号 |
| 翻页 | 4 张固定素材：掉电中 / 心飞扬 / 咖啡与我 / 信息流 |
| 图片 | 设计库原图，按 4:3 缩放到 236 宽（存 152×114，显示时放大） |
| 标题 | App 下发的独白标题，22 号 @188 |
| 锁徽标 | 同皮肤页逻辑 @180 |
| 操作 | 解锁款显示「阅读」；未解锁隐藏 |

阅读页：图 236 宽 @88 + 标题 22 号 @300 + 尾注「这段话，一直留在这里」16 灰 @384，右上角关闭回列表。

**成就（原「动态」，显示标题 + 正文）**

| 元素 | 说明 |
|---|---|
| 标题 | 成就，22 号 |
| 卡片 | 淡黄便签 330×260，一屏一条，最多 2 条（`INSIGHT_COUNT = 2`，与 App `InsightEngine.maxShown` 对齐） |
| 标题 | 22 号 @42（卡内） |
| 正文 | 16 号 @96（卡内），来自 `CONTENT_SYNC.text` |
| 兜底 | 正文为空时显示「详情请返回APP查看」 |

成就**不做翻页锁**：App 只下发存在的条目，固件只翻 `present=1` 的。

### 4.3 演示页与测试流程

| 入口 | 行为 |
|---|---|
| 演示 | 进「脑力演示」页：大字脑力值 44 + 脑子 236² + 状态胶囊；**轻点脑子循环 6 档**（100→0→20→40→60→80），不写 KV、不发给手机 |
| 测试流程 | 直接以 30 s 起一次专注，**置 `ui.testing = true`，所有上行事件被 `emit()` 拦掉**，手机端看不到这一轮会话 |

两个入口都只在本地点按，不产生真实业务数据；测试流程是给产线/研发走 UI 用的。

### 4.4 数据契约

| 方向 | 帧 | 字段 | 含义 |
|---|---|---|---|
| App→设备 | `STATE_SYNC` | `unlockedSkins`（bitmask，8 款） | 已解锁皮肤集合 |
| | | `wearingSkin`（字符串，空=未穿） | 当前穿戴；设备侧 `NO_SKIN=0xFF` |
| | | `brainScore` | 脑力档位 0/20/40/60/80/100 |
| App→设备 | `CONTENT_SYNC` | `contentKind="postcard"`, `index`, `present`, `title`, `text`, `img` | 独白 4 条；`present`=是否解锁，`img` 保留字段但**已不被使用**（改为固定设计图） |
| | | `contentKind="insight"`, `index`, `present`, `title`, `text` | 成就；`text` = 正文 |
| 设备→App | `SKIN_SET_REQUEST` | `detail = sleep/love/...` 或 `""` | 板子上穿/脱皮肤，App 需同步经济状态 |

**App 侧配套（本轮新增）**：`HardwareSync.syncContent()` 原本只给洞察发 `title`，注释还写着「面板不接收洞察正文」。现改为发 `title + text`，并对超过 **250 B** 的帧逐字裁剪；同时**锁定独白也带标题**，否则锁着的 4 张图下方是空白。

### 4.5 持久化

| Key | 值 | 写入时机 |
|---|---|---|
| `brain_skin` | 0..7 或 `0xFF` | 穿/脱皮肤；`STATE_SYNC` 发现与本地不一致时 |

上电 `brain_ui_init()` 读回，断网重启仍记得上次穿的那件。

---

## 5. BLE 通信规范

### 5.1 广播与服务

| 项目 | 值 |
|---|---|
| 广播名 | `SlowHouse-Brain`（放在 adv 的 0x09 完整名里，共 20 B） |
| 广播数据 | `0x02 0x01 0x06`（LE 通用可发现）+ `0x10 0x09 "SlowHouse-Brain"` |
| 扫描响应 | `0x11 0x07` + 128-bit 服务 UUID |
| 服务 UUID | `12345678-1234-5678-9ABC-DEF012345601` |
| 写入特征 | `…5602`（Write / Write Without Response） |
| 通知特征 | `…5603`（Notify） |
| MTU | 协商到 256，实测 App 侧单帧上限取 **250 B** |

### 5.2 帧格式

```json
{
  "protocolVersion": 1,
  "eventID": "dev-00008d9a",
  "sessionID": "",
  "eventType": "STATE_SYNC",
  "timestamp": 36,
  "payload": { }
}
```

- 设备侧接收缓冲 **384 B**（`brain_write_access` 栈上 `uint8_t frame[384]`），上行缓冲 320 B。
- 所有字符串 ID 最长 40 B（`BRAIN_ID_LEN`）。

### 5.3 上行事件

| 事件 | 触发 | 说明 |
|---|---|---|
| `STATE_REQUEST` | 连上并订阅通知后 | 让 App 推一次完整快照 |
| `TIME_UP` | 本地倒计时归零 | — |
| `END_REQUEST` | 长按「提前结束」满 2 s | — |
| `BREAK_ACTION` | 板子上选了小事 | `payload.detail = water/move/sleep/return` |
| `SKIN_SET_REQUEST` | 板子上穿/脱皮肤 | `payload.detail = 皮肤名` 或空串 |

### 5.4 ACK 与能力协商

设备对每次带 `eventID` 的写入都回一个 `STATE_ACK`：

```json
{"protocolVersion":1,"eventType":"STATE_ACK","sessionID":"…","timestamp":…,
 "payload":{"ackEventID":"…","status":0,"brainScore":80,
            "capabilities":["CONTENT_SYNC","SKIN_SET"]}}
```

| status | 含义 |
|---|---|
| `0x00` | 成功 |
| `0x01` | 长度非法 |
| `0x02` | 脑力值非法（必须 0/20/40/60/80/100） |

`capabilities` 是**向后兼容的护栏**：App 收到能力声明前不发内容类帧，因此新版 App 连老固件也安全。

### 5.5 并发与幂等约束

| 约束 | 实现 | 为什么 |
|---|---|---|
| 重复事件不重放 | 6 槽 `eventID` 环形表，命中就只回 ACK | 重连补发不能让页面再切一次 |
| 单条在途发送 | `g_tx_busy` + 工作队列 | 两个任务共用一个 `g_tx_buf` 会串包 |
| 单条在途写入（App 侧） | `HardwareLink.pendingWrites` | 连续多条 `withResponse` 会打爆 NimBLE 动态 mbuf 池 |
| 内容帧独立发送 | 每条 `CONTENT_SYNC` 单独一帧且 ≤250 B | 固件接收缓冲 384 B，且 ATT 单写有上限 |

---

## 6. 资源管线（图片 / 字体 / 语音）

### 6.1 图片（20 个 `.c`）

| 类别 | 数量 | 存储尺寸 | 颜色格式 | 单张体积 |
|---|---|---|---|---|
| 六档状态脑 | 6 | 236×236 | RGB565A8 | 167,088 B |
| 小事图（水/走/睡） | 3 | 236×236 | RGB565A8 | 167,088 B |
| 皮肤形象 | 8 | 160×160 | RGB565A8 | 76,800 B |
| 独白设计图 | 4 | 152×114 | RGB565A8 | 51,984 B |

- `RGB565A8` = RGB565 像素面 + 8 bit alpha 面，共 3 B/px。
- **所有页面 `BRAIN_SIZE = 236`**，缩放倍率由 `image_zoom_for()` 按源图宽度换算 —— 不能写死 `lv_image_set_scale`，否则 160 图会被缩成 94 px。
- 状态脑已从 256² **重采样到 236²**：省 236 KB flash，且绘制变成 1:1 不再重采样。
- 皮肤形象仍是 160² → 显示时放大 1.475×（8 张都拉到 236² 还要多占约 380 KB，放不下，见 §8.1）。
- `brain_sleep.c` 一份资源两用：既是「安睡脑」皮肤，也是小事里的「美美睡一觉」。

### 6.2 字体（五档，按字号独立字表）

| 字号 | 用途 | 字形数 |
|---|---|---|
| 16 | 副文案、按钮文字、独白正文、成就正文 | ~448（含历史冗余） |
| 18 | 小事页尾注「15秒后，我回到这里陪你」 | 10 |
| 22 | 页面标题、皮肤名、独白标题、成就标题 | 216 |
| 30 | 状态文案、小事文案 | 47 |
| 44 | 「脑力 N」与倒计时 | 2 汉字 + 数字 |

- 18/30/44 **只带各自固定文案的字**，这是被 flash 逼出来的（44 号曾从 385 KB 降到 6 KB）。
- 生成：`python3 tools/regen_font.py`（16/22 两档；16 会继承上一版字符集）。
- 校验：`python3 tools/check_font_coverage.py` —— 按字号解析 `label()` / `wrapped_label()` 实参 + App 动态文案，逐字比 cmap，缺字直接报。**这轮靠它抓出「与」（咖啡与我）缺字。**
- 14/36 号字库仍在目录里但被 CMake 排除，`-Werror` 下别引用。

### 6.3 语音（两段，16 kHz 单声道 16 bit）

| 文件 | 内容 | 时长 | 触发 |
|---|---|---|---|
| `assets/five.wav` | 最后 5 秒提示 | 4.03 s | `remaining == 5` |
| `assets/done.wav` | **我陪你看看其他事情吧～** | 2.33 s | 倒计时归零 |

- 生成：`say -v Tingting -o x.aiff "文案"` → `afconvert -f WAVE -d LEI16@16000 -c 1` → `python3 tools/generate_voice.py assets src/voice_clips.c`
- **改了 WAV 必须重跑 `generate_voice.py`**，它是把 PCM 直接烘进 `voice_clips.c` 的；只换 WAV 不重跑等于没改。
- 播放侧 codec **常开不复开**：T5AI 音频驱动二次 `tdl_audio_open` 会 `aud_adc_init` 失败并 fault。

---

## 7. 编译与烧录（关键步骤）

### 7.1 环境

```bash
cd /Users/tanshinan/Projects/TuyaOpen-160
source ./export.sh          # 注入工具链与 ARMINO 环境变量
```

### 7.2 编译

```bash
cd apps/brain_companion
../../.venv/bin/python ../../tos.py build
```

> **必须让构建进程能自由删除文件**。受限沙箱里 ninja 无法重写 `.ninja_log`（报 `failed recompaction: Operation not permitted`），cmake 也无法清理临时文件。这一条踩了很久。

产物：`apps/brain_companion/.build/bin/brain_companion_QIO_1.0.0.bin`（同时同步到 `dist/brain_companion_1.0.0/`）。

### 7.3 烧录

```bash
cd /Users/tanshinan/Projects/TuyaOpen-160
.venv-flash/bin/python tools/tyutool/tyutool_cli.py write -d T5AI \
  -f apps/brain_companion/.build/bin/brain_companion_QIO_1.0.0.bin \
  -p /dev/cu.usbmodem5B220305451 -b 2000000
```

| 项 | 值 |
|---|---|
| 下载口 | `/dev/cu.usbmodem5B220305451` |
| 日志口 | `/dev/cu.usbmodem5B220305453`（460800，空闲时基本无输出） |
| 波特率 | 2,000,000（实测最快且稳定） |
| 时长 | 约 43 s 写入 + 校验 |

> `tos.py flash` 在这台机器上会失败（它调系统 python，缺 `click`）。用上面的 `.venv-flash` 直接调 tyutool。
> 成功标志：`Write flash success` → `CRC check success` → `Reboot done`。

### 7.4 验证

```bash
# BLE 全链路回归：连接 → 6 档写入 → ACK → 非法值 → 断连重连
cd apps/brain_companion && swift tools/ble_probe.swift

# 全流程回归（两主流程一起跑）
swift tools/full_flow_probe.swift
```

本轮结果：`ble_probe` **PASSED**（6.0 s，7 个 ACK，广播帧 1）。唯一一条 `MISMATCH` 是首帧收到的 `STATE_REQUEST` JSON，属探针设计预期。

### 7.5 归档

```bash
cp .build/bin/brain_companion_QIO_1.0.0.bin ~/Projects/brain_companion_v20_collections_ui.bin
shasum -a 256 .build/bin/brain_companion_QIO_1.0.0.bin
```

---

## 8. 已知约束与踩坑记录

### 8.1 Flash 只剩 78 KB（首要约束）

- 本轮加 4 张独白图 + 字体扩字，AP 直接溢出 **157,412 B**。
- 解法：`tools/shrink_brain_images.py` 把 8 张 256² 脑子图重采样到 236²（脚本能直接解 C 数组，不需要源图），省 236,160 B。
- 下一步腾空间的最优解：把 `brain_font_16` 的 448 字形裁到实际用量，可再省约 40–45 KB。裁之前**先跑 `check_font_coverage.py`**，否则动态文案会开始出豆腐块。

### 8.2 NimBLE 动态内存池缺陷（已修，勿回退）

现象：App 连续多条 `withResponse` 写入 → 串口 `ERR IN MEM GET : ble_hci_dyna_ram_acl_pool` → `AP crash rr 0x12` → 反复重连 → 屏幕闪。

根因：动态池只记账 `mp_num_free`，free 链表为空，host 仍用静态 `os_mbuf_get` 取，返回 NULL 后 assert。

修法：`tuya_ble_mbuf.c` 的 `os_mbuf_get()` 在 `#if TUYA_USE_DYNA_RAM` 下检测到 `mp_flags>0` 时改走堆分配 `os_dyna_mbuf_get()`；**必须**同时在 `nimble/include/tuya_ble_mbuf.h` 补前置声明，否则 `-Werror` 下 implicit declaration 直接编译失败。

### 8.3 其它必须知道的坑

| 坑 | 表现 | 处理 |
|---|---|---|
| AP 核缓存指向旧路径 | 链接报 `lib/libtuyaapp.a missing and no known rule` | 删 `platform/T5AI/t5_os/build/bk7258/tuya_app/bk7258_ap` 重新生成 |
| `diff2ya` 覆盖复制被拦 | `PermissionError: Brokered host copy overwrite refused` | 先 `rm` 目标文件，让复制变成新建 |
| 字体缺字 | 屏上出现豆腐块 | 跑 `check_font_coverage.py`，补 `regen_font.py` 字表 |
| 图片缩放写死 | 160 图被缩到 94 px | 一律走 `image_zoom_for()` / `fit_brain_image()` |
| 翻页容器裁子元素 | 文案改大后直接消失（小事页踩过） | 翻页区高度必须容得下图 + 文案 |
| 音频二次 open | `aud_adc_init` 失败后 fault | codec 常开，只 `play_stop` 清队列 |
| 断连重建实时页 | 每次重连闪屏 | 连接回调只更新状态，不重建 LVGL 树 |

### 8.4 已知取舍

1. **提前结束无二次确认**：2 s 长按代替确认页，误触概率靠时长兜住；确认权从硬件收回到了硬件。
2. **皮肤形象 160² 放大显示**：flash 限制所致，观感略软，暂不回退到 256。
3. **独白 `img` 字段保留但不生效**：改为固定 4 张设计图，协议不破坏。
4. **成就最多 2 条**：与 App `InsightEngine.maxShown` 对齐，扩大需同步改两侧。

---

## 9. 真机验收清单

| # | 场景 | 预期 |
|---|---|---|
| 1 | 上电 | 实时页显示脑力 80 与对应脑子，2.8 s 呼吸 |
| 2 | App 改脑力 | 6 档图与文案即时切换，不闪屏 |
| 3 | 轻点屏幕空白 | 5 个图标展开为「演示/测试流程/皮肤/独白/成就」 |
| 4 | 静止 10 s | 自动收回图标态 |
| 5 | 点「皮肤」 | 8 款全展示，未解锁右上角 ✕ 且无「穿上」 |
| 6 | 穿 A 再穿 B | A 自动换下，B 常亮，断电重启仍是 B |
| 7 | 点「独白」 | 4 张设计图 + 标题，锁着的也能看到图与标题 |
| 8 | 点「成就」 | 便签卡片显示标题 + 正文 |
| 9 | 点「测试流程」 | 直接 30 s 专注，手机端收不到任何事件 |
| 10 | 点「演示」 | 循环 6 档，返回后脑力未变 |
| 11 | 开始专注 | 圆环消减、倒计时刷新 |
| 12 | 剩 5 s | 语音 + 加快呼吸 + 背景脉动 |
| 13 | 到点 | 播「我陪你看看其他事情吧～」→ 直接进小事选页 |
| 14 | 小事左右滑 | 4 项都能选，OK 钮可点，15 s 自动回实时页 |
| 15 | 长按「提前结束」2 s | 无确认页，直接进小事选页，手机收到 `END_REQUEST` |
| 16 | 断连 | 提示切换，最后脑力保留，可本地操作 |
| 17 | 重连 | 自动恢复当前会话，不闪屏、不重启 |

---

## 10. 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-11-06 | v1 | 第一主流程闭环：六档脑力、专注环、到点语音、小事 |
| 2026-11-07 | v1.1 | 第二主流程：换装/独白/动态；修 NimBLE mbuf 池崩溃；换装可逆 + KV |
| 2026-11-08 | v1.2 | 提前结束去确认页；到点语音换词；皮肤/独白/成就改名；独白换 4 张设计图；5 按钮图标↔文字；脑子统一 236 并重采样省 236 KB |
| 2026-11-11 | v1.2-doc | 本文档；App 侧补发洞察正文与锁定独白标题 |

---

_文档由固件源码直接归纳，涉及的路径、常量、SHA256 均可在仓库与 `apps/brain_companion/` 中逐项核对。_

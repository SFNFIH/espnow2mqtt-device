# 状态流转

这是理解 `en2m` 最重要的一篇。它按时间顺序讲清楚：**一个值从哪来、经过什么、到哪去**。

所有时序图里的 `[en2m]` 表示这一步跑在 en2m 任务上，`[wifi]` 表示跑在 Wi-Fi 任务
（ESP-NOW 收包回调）上，`[evt]` 表示跑在 `esp_event` 默认循环任务上，
`[isr]` 表示中断上下文，`[app]` 表示调用者自己的任务。上下文规则见 [concurrency.md](concurrency.md)。

- [1. 启动时序](#1-启动时序)
- [2. 属性状态机](#2-属性状态机)
- [3. 传感器上报流水线](#3-传感器上报流水线)
- [4. 命令下行流水线](#4-命令下行流水线)
- [5. 本地控制流水线](#5-本地控制流水线)
- [6. mesh 入网状态机](#6-mesh-入网状态机)
- [7. 下行 ACK / 重传状态机](#7-下行-ack--重传状态机)
- [8. 持久化生命周期](#8-持久化生命周期)
- [9. Identify 倒计时](#9-identify-倒计时)
- [10. 100 ms 维护 tick 里到底做了什么](#10-100-ms-维护-tick-里到底做了什么)
- [11. 停止时序](#11-停止时序)

---

## 1. 启动时序

`app_main` 里的顺序是**有要求的**：数据模型必须在 `en2m_start` 之前建好。

```c
void app_main(void)
{
    /* ① 先初始化你自己的硬件 */
    ESP_ERROR_CHECK(drv_gpio_relay_init(PIN_RELAY, true));

    /* ② 再建数据模型（必须在 en2m_start 之前） */
    en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG);

    /* ③ 最后启动 */
    en2m_device_config_t cfg = { ... };
    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

`en2m_start` 内部的完整顺序：

```
en2m_start(config)                                                    [app]
│
├─ 参数校验：config != NULL，!s_model.started
│
├─ 填默认值
│    report_interval_ms == 0  → LEAF 用 30000，其他用 15000
│    min_report_interval_ms == 0 → 1000
│    s_model.cfg = cfg;  s_model.configured = true
│        ↑ 必须在 mesh_init 之前，因为 mesh_init 要读 task_stack_size/priority
│
├─ en2m_mesh_init(&cfg.mesh)
│    ├─ 校验 role ∈ {COORDINATOR, ROUTER, LEAF}，!s_ctx.inited
│    ├─ memset(&s_ctx)；拷配置；channel == 0 → EN2M_WIFI_CHANNEL
│    ├─ name 拷进 s_ctx.name[16]；path_cost = 255；next_cmd_id = 1
│    ├─ 建互斥锁；建队列（EN2M_QUEUE_LEN × sizeof(en2m_item_t)）
│    ├─ nvs_flash_init()（遇 NO_FREE_PAGES / NEW_VERSION_FOUND 自动 erase 后重试）
│    ├─ esp_netif_init()                 ← ESP_ERR_INVALID_STATE 视为成功
│    ├─ esp_event_loop_create_default()  ← ESP_ERR_INVALID_STATE 视为成功
│    ├─ Wi-Fi：init → storage=RAM → mode=STA → start → set_channel
│    │      注意：只开 STA 并锁信道，从不连接任何 AP
│    ├─ esp_wifi_get_mac(WIFI_IF_STA) → s_ctx.self_mac
│    ├─ esp_now_init() → esp_now_register_recv_cb(en2m_espnow_recv_cb)
│    ├─ 把广播地址加成 ESP-NOW peer
│    ├─ 若 role == COORDINATOR：path_cost = 0，has_parent = true（永远是树根）
│    ├─ inited = true；running = true
│    └─ xTaskCreate(en2m_task, "en2m", stack, NULL, priority, &s_ctx.task)
│           ↑ 任务从这一刻开始跑，但 s_model.started 还是 false，
│             所以 en2m_model_tick() 会立即 return，不会提前上报
│
├─ en2m_dm_restore()            从 NVS 把 persist 属性读回属性存储
│
├─ en2m_model_apply_persisted() 对每个 persist 属性调 en2m_attribute_write()
│     → 走 write 回调 → 硬件恢复到断电前的状态
│     注意：restore 已经把值塞进槽位了，所以紧接着的 en2m_attribute_set
│     发现值没变，en2m_model_on_change 压根不会被调用——
│     恢复期没有 attribute_changed、没有事件、没有上报
│     详见 persistence.md
│
├─ last_report_ms = now；last_persist_ms = now
├─ started = true
├─ next_report_ms = now + 500   ← 开机 500 ms 后发第一份状态
│
└─ 发出 EN2M_EVENT_STARTED
```

`app_main` 返回后，一切由 en2m 任务驱动。

### 为什么第一份上报要等 500 ms

因为此刻还没有父节点。`en2m_send_uplink` 对 `EN2M_MSG_STATE` 有一条硬规则：
**没有父节点就直接返回 `ESP_ERR_INVALID_STATE`，不发**（只有 HELLO 和 HEARTBEAT
在无父时会走广播）。500 ms 给选父留了时间；而且一旦真的选上父节点，
`en2m_model_on_link_change(true)` 会把 `next_report_ms` 改成 `now + 200`，
所以入网后 200 ms 就有状态，不用等周期。

---

## 2. 属性状态机

每个属性是一个 `en2m_attr_slot_t`：

```c
typedef struct {
    bool used;            /* 槽位已分配 */
    uint16_t id;          /* attribute id */
    bool persist;         /* 建的时候声明的，运行时不变 */
    bool persist_dirty;   /* 值变过、还没写进 NVS */
    en2m_value_t value;   /* 带 tag 的当前值 */
} en2m_attr_slot_t;
```

有两条**故意不等价**的写入路径。选错了行为就不对。

```
        ┌──────────────────────────┐         ┌──────────────────────────┐
        │  en2m_attribute_set      │         │ en2m_attribute_write     │
        │  「硬件现在就是这个值」  │         │ 「请把硬件变成这个值」   │
        │  传感器用                │         │ 执行器用                 │
        └────────────┬─────────────┘         └────────────┬─────────────┘
                     │                                    │
                     │                        ┌───────────▼────────────┐
                     │                        │ 取 target = coerce(值) │
                     │                        │ 取 cluster->write_cb   │
                     │                        └───────────┬────────────┘
                     │                                    │
                     │                        ┌───────────▼────────────┐
                     │                        │ ① cluster 级 write_cb  │
                     │                        │ ② 设备级 attribute_write│
                     │                        │   （①返回 NOT_SUPPORTED│
                     │                        │     时才试②）          │
                     │                        └───────────┬────────────┘
                     │                                    │
                     │                     err == OK 或 NOT_SUPPORTED？
                     │                        ┌───────────┴────────────┐
                     │                        │ 否 → 记 WARN，直接返回 │
                     │                        │      **不提交、不上报**│
                     │                        └───────────┬────────────┘
                     │                                 是 │
                     └──────────────┬─────────────────────┘
                                    │
                     ┌──────────────▼──────────────┐
                     │ 提交（持锁）                │
                     │  committed = coerce(值)     │
                     │  和旧值相等？ → 什么都不做  │
                     │  不等 → 写入槽位            │
                     │         persist_dirty=persist│
                     └──────────────┬──────────────┘
                                    │ 只有真的变了才继续
                     ┌──────────────▼──────────────┐
                     │ en2m_model_on_change()      │
                     │  1. 发 ATTRIBUTE_UPDATED 事件│
                     │  2. 调 attribute_changed 回调│
                     │  3. 模式 DEFAULT / ON_CHANGE │
                     │     → en2m_report_schedule(0)│
                     └─────────────────────────────┘
```

### 三个必须记住的性质

**① 去重在存储层**。值没变就没有回调、没有事件、没有上报。
所以一个每 100 ms 采样、读数基本不动的传感器**不会**刷爆 mesh——
但这不是限流，真正的限流是 `min_report_interval_ms`，见 [reporting.md](reporting.md)。

**② 类型强制**。属性建的时候声明了类型，之后写进来的值会被 `en2m_value_coerce`
重新塑形成那个类型：

```c
/* 属性声明为 EN2M_VAL_U8（CURRENT_LEVEL） */
en2m_attribute_set(1, EN2M_CLUSTER_LEVEL_CONTROL, EN2M_ATTR_CURRENT_LEVEL, en2m_i32(200));
/* 存进去的是 en2m_u8(200)，不是 i32 */
```

好处是驱动可以传自己最自然的整数宽度，不用记住属性声明成什么。
代价是**溢出会被静默截断**（`en2m_i32(300)` 写进 u8 属性会变成 44），
所以自己 clamp 好范围。

**③ write 回调失败就不提交**。这是"不上报没到达的状态"的保证。
回调返回 `ESP_FAIL` 时，属性保持旧值，HA 里看到的也还是旧值。

### `ESP_ERR_NOT_SUPPORTED` 和"裸提交"

如果 cluster 回调和设备回调都返回 `ESP_ERR_NOT_SUPPORTED`（或者干脆没注册回调），
`en2m_attribute_write` **仍然会提交**。这是有意的：对于纯软件状态
（比如一个没有物理执行器的虚拟开关），"没人管"就等于"直接接受"。

---

## 3. 传感器上报流水线

传感器有两种写法，取决于数据是**推**过来的还是要**拉**的。

### 3.1 推式（中断、外部事件）

门磁、按键、PIR 这类：

```
GPIO 边沿                                                          [isr]
│
├─ 你的 ISR
│    en2m_attribute_set_from_isr(1, BOOLEAN_STATE, STATE_VALUE,
│                                en2m_bool(closed), &woken)
│      → 组装 EN2M_ITEM_ATTR，xQueueSendFromISR
│    if (woken) portYIELD_FROM_ISR();
│
▼ ······················· 队列 ·······················
│
├─ en2m_task 收到 EN2M_ITEM_ATTR                                   [en2m]
│    → en2m_attribute_set(...)      ← 真正的提交在任务上发生
│        → 变了 → en2m_model_on_change()
│            → ATTRIBUTE_UPDATED 事件
│            → attribute_changed 回调
│            → en2m_report_schedule(0)
│
└─ 下一个 100 ms tick：now >= next_report_ms → refresh + transmit
```

注意 ISR 里**不要**调 `en2m_attribute_set`（它会拿互斥锁），必须用 `_from_isr` 版本。

### 3.2 拉式（I2C、一线、ADC）

温湿度、光照这类需要主动采样的：

```
每 100 ms tick，判断该上报了                                       [en2m]
│
├─ en2m_dm_refresh()
│    遍历 4×8×6 个槽位，对每个 used 的属性：
│      持锁 → 抄下 (path, 当前值, cluster->read_cb, ctx) → 解锁
│      cluster 没有 read_cb 就用设备级 cfg->attribute_read + user_ctx
│      都没有 → 跳过
│      err = cb(&path, &value, ctx)          ← 回调在锁外跑，可以走 I2C
│      err == ESP_OK → en2m_attribute_set(path, value)
│                        （值一样就什么都不发生）
│
└─ en2m_report_transmit()
```

所以一个纯拉式传感器的应用代码只有一个函数：

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_TEMPERATURE_MEASUREMENT) {
        float t;
        if (drv_dht_read(&t, NULL) != ESP_OK) {
            return ESP_ERR_NOT_SUPPORTED;   /* 保留上次的缓存值 */
        }
        *out = en2m_i16((int16_t)(t * 100));
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
```

**关键**：`en2m_dm_refresh` 是**每次上报前**跑的，不是每个 tick 跑。
采样频率因此等于上报频率，由 `report_interval_ms` 和 `min_report_interval_ms` 控制。
DHT22 有 2 秒最小采样间隔的硬约束，所以 `th_sensor` 示例把
`min_report_interval_ms` 设成 5000。

### 3.3 上报组装与发送

```
en2m_report_transmit()                                             [en2m]
│
├─ 三档降级，取第一个 ≤ 160 字节的
│    档 1  node_role + caps + 全部值
│    档 2  caps + 全部值
│    档 3  只有值
│
├─ 三档都超长 → 取档 3 的前 160 字节，truncated = true，打 ESP_LOGE
│     （这是唯一会发出非法 JSON 的情况，正常设备不会碰到）
│
├─ en2m_send_uplink(EN2M_MSG_STATE, 0, json, len)
│    ├─ role == COORDINATOR → ESP_ERR_NOT_SUPPORTED
│    ├─ !has_parent → ESP_ERR_INVALID_STATE（STATE 不广播）
│    ├─ 填帧：seq++、cost = path_cost、origin = self、dest = 广播、via = self
│    └─ esp_now_send(parent_mac, pkt)
│
├─ last_report_ms = now；next_report_ms = 0
└─ 发 EN2M_EVENT_REPORT_SENT { length, truncated, err }
```

`err` 是原样传出来的，所以监听 `REPORT_SENT` 就能知道上报有没有真的出去。

---

## 4. 命令下行流水线

HA 点一下开关，到继电器动作，全程是这样：

```
HA  →  MQTT espnow2mqtt/<slug>/set  {"switch":"ON"}
       │
Bridge →  USB NDJSON {"type":"cmd","mac":"..","id":7,"payload":{"switch":"ON"}}
       │
S3 协调器 en2m_send_downlink(mac, 7, payload)
       │   ├─ 填 CMD 帧，dest = 设备 MAC
       │   ├─ next = lookup_route(dest) 或 dest 本身
       │   ├─ esp_now_send(next, pkt)
       │   └─ cmd_id != 0 → pending_add(dest, 7, pkt)   ← 开始等 ACK
       │
       ▼ ESP-NOW（可能经 router 转发，hop++，via 改写）
       │
C3 设备
│
├─ en2m_espnow_recv_cb                                             [wifi]
│    仅：校验长度 → 拷进 en2m_item_t → xQueueSend(超时 0)
│    队列满 → rx_dropped++，后续由 tick 报成 EN2M_EVENT_RX_DROPPED
│
▼ ······················· 队列 ·······················
│
├─ en2m_dispatch_rx                                                [en2m]
│    ├─ 持锁 en2m_handle_rx()
│    │    ├─ magic != 0xA5 或 version != 2 → 丢
│    │    ├─ 源就是自己 → 丢
│    │    ├─ en2m_remember_neighbor(from, role, cost, rssi)
│    │    └─ msg_type == CMD 且 dest == self_mac
│    │         → outcome.deliver_command = true
│    └─ 解锁，然后才调应用侧
│         config.on_command 非空 → 调它（裸帧，自己解析）
│         为空（设备固件的常态）→ en2m_model_on_command_frame(pkt)
│
├─ en2m_model_on_command_frame                                     [en2m]
│    ├─ !started → 直接 return（**不回 ACK**，协调器会重传到超时）
│    ├─ payload 拷进 char[161]，cJSON_Parse
│    ├─ 解析失败 → WARN，return（**不回 ACK**）
│    ├─ 读 "ep" → endpoint_id（没有就是 0）
│    ├─ 分两种形态解码：
│    │    A. cluster 形态：有 "cluster" + "command" 且 cluster 名认识
│    │    B. 扁平形态：否则走 en2m_model_decode_flat()
│    │       认出 0 条 → WARN「没有本设备能理解的东西」
│    ├─ cmd_id != 0 → en2m_send_uplink(EN2M_MSG_ACK, cmd_id, NULL, 0)
│    │     ↑ 只要 JSON 解析成功就回 ACK，哪怕内容一条都没认出来。
│    │       这样协调器不会为一个无意义的 payload 白重传三次。
│    └─ en2m_report_schedule(0)   ← 让 HA 立刻看到新状态
│
└─ ACK + 新 STATE 上行 → 协调器 pending_resolve → EN2M_EVENT_ACK_RECEIVED
                                                 → USB {"type":"ack","ok":true}
```

### 单条命令的派发（`en2m_model_exec`）

```
endpoint_id == 0 ？ → en2m_model_endpoint_with(cluster_id)
                       取**编号最小**的、拥有该 cluster 的 endpoint
                       仍然是 0 → WARN「没有 endpoint 暴露这个 cluster」+ NOT_FOUND
│
├─ 发 EN2M_EVENT_COMMAND_RECEIVED
│
├─ ① cluster 级 command_cb（en2m_cluster_set_command_cb 注册的）
├─ ② 设备级 cfg.command                  ← ① 返回 NOT_SUPPORTED 才试
├─ 任一返回非 NOT_SUPPORTED → 就此结束，返回它的 err
│
└─ ③ 内建翻译成属性写
```

### 内建翻译表

这是第 ③ 级，也是大多数设备实际走的路径。**注意它全部用 `en2m_attribute_write`**，
也就是会穿过 write 回调。

| 命令 | 内建行为 |
|---|---|
| `ON` / `OFF` | 写 `ON_OFF` = true/false |
| `TOGGLE` | 先读 `ON_OFF`，再写反值 |
| `MOVE_TO_LEVEL` | clamp 0–254 写 `CURRENT_LEVEL`；成功且 level > 0 且存在带 OnOff 的 endpoint → **顺带把 OnOff 写成 true** |
| `MOVE_TO_COLOR_TEMPERATURE` | 写 `COLOR_TEMPERATURE_MIREDS` |
| `LOCK_DOOR` / `UNLOCK_DOOR` | 写 `LOCK_STATE` |
| `UP_OR_OPEN` | 写位置 = 0（全开） |
| `DOWN_OR_CLOSE` | 写位置 = 100（全闭） |
| `GO_TO_LIFT_PERCENTAGE` | clamp 0–100 写位置 |
| `STOP_MOTION` | **无内建行为**，打 WARN 并返回 NOT_SUPPORTED。只有应用知道电机停在哪，必须自己实现 `command` 回调 |
| `SET_FAN_MODE` | 写 `FAN_MODE`；若模式是 OFF → 顺带把 `PERCENT_SETTING` 写成 0 |
| `SET_FAN_PERCENT` | clamp 0–100 写 `PERCENT_SETTING`；然后读回 `FAN_MODE`：百分比为 0 且模式非 OFF → 写 OFF；百分比 > 0 且模式是 OFF → 写 ON |
| `SET_SYSTEM_MODE` | 写 `SYSTEM_MODE` |
| `SET_HEATING_SETPOINT` / `SET_COOLING_SETPOINT` | 写对应 setpoint（0.01 °C） |
| `IDENTIFY` | 用 `en2m_attribute_set`（不是 write）把 `IDENTIFY_TIME` 设为秒数，发 `EN2M_EVENT_IDENTIFY`，调 `identify` 回调 |
| 其他 | WARN + NOT_SUPPORTED |

"顺带"那几条是为了对上 HA 的直觉：HA 拖亮度滑条时只发 `brightness`，
用户的意思显然是"开灯并调到这个亮度"。

### 扁平形态的解码顺序

一条 payload 里可以带多个键，按这个顺序依次识别，每识别一个就派发一条命令：

| JSON 键 | 目标 cluster | 命令 |
|---|---|---|
| `switch`: `"ON"` / `"OFF"` / `"TOGGLE"` | OnOff | ON / OFF / TOGGLE |
| `brightness`（或 `level`） | LevelControl | MOVE_TO_LEVEL |
| `color_temp` | ColorControl | MOVE_TO_COLOR_TEMPERATURE |
| `position` | WindowCovering | GO_TO_LIFT_PERCENTAGE |
| `cover`: `"OPEN"` / `"CLOSE"` / 其他 | WindowCovering | UP_OR_OPEN / DOWN_OR_CLOSE / STOP_MOTION |
| `lock`: `"LOCK"` / 其他 | DoorLock | LOCK_DOOR / UNLOCK_DOOR |
| `fan_mode` | FanControl | SET_FAN_MODE |
| `percentage` | FanControl | SET_FAN_PERCENT |
| `hvac_mode` | Thermostat | SET_SYSTEM_MODE |
| `target_temperature` | Thermostat | 读当前 `SYSTEM_MODE`：`cool` → SET_COOLING_SETPOINT，否则 SET_HEATING_SETPOINT |
| `identify` | Identify | IDENTIFY |

`position` 和 `cover` 是互斥的：有 `position` 就不看 `cover`。

### cluster 形态

```json
{"ep":1,"cluster":"level_control","command":"move_to_level","level":200}
```

认识的 cluster 名只有这 8 个（纯传感器 cluster 没有命令，所以不在表里）：
`identify`、`on_off`、`level_control`、`color_control`、`door_lock`、
`window_covering`、`thermostat`、`fan_control`。

命令名与参数键：

| cluster | command | 参数键 | 单位 |
|---|---|---|---|
| `on_off` | `on` / `off` / `toggle` | — | |
| `level_control` | `move_to_level` | `level` | 0–254 |
| `color_control` | `move_to_color_temperature` | `color_temp` | mired |
| `door_lock` | `lock` / `unlock` | — | |
| `window_covering` | `open` / `close` / `stop` / `go_to` | `position` | 0–100（闭合百分比） |
| `fan_control` | `set_mode` / `set_percent` | `mode` / `percentage` | 字符串 / 0–100 |
| `thermostat` | `set_mode` / `set_heating` / `set_cooling` | `mode` / `temperature` | 字符串 / °C（内部 ×100） |
| `identify` | `identify` | `seconds` | 秒，缺省 10 |

---

## 5. 本地控制流水线

物理按键、场景、定时器要改状态时，**不要直接调驱动**。调 `en2m_attribute_write`，
让它走和远程命令完全相同的那条路：

```
按键中断                                                            [isr]
│
├─ en2m_schedule_from_isr(toggle, NULL, &woken)
│    → EN2M_ITEM_WORK 入队
│
▼ ······················· 队列 ·······················
│
├─ en2m_task 执行 toggle()                                          [en2m]
│    ├─ en2m_attribute_get(1, ON_OFF, ON_OFF, &cur)
│    └─ en2m_attribute_write(1, ON_OFF, ON_OFF, en2m_bool(!cur.v.b))
│         → write 回调 → 继电器动作
│         → 提交 → on_change → 安排上报
│
└─ HA 里的开关跟着翻，因为走的是同一条路
```

这就是 `examples/relay_switch` 的全部逻辑。**直接调 `drv_gpio_relay_set()`
是反模式**：硬件变了但属性存储不知道，HA 会一直显示旧状态，
而且下次远程命令会从错的状态开始 toggle。

---

## 6. mesh 入网状态机

协调器永远是树根，`en2m_mesh_init` 里就把它设成 `path_cost = 0, has_parent = true`，
不参与选父。Leaf 和 Router 的状态机是这样：

```
        ┌─────────────────────────────┐
        │  NO_PARENT                  │
        │  has_parent = false         │  ← 初始状态
        │  path_cost  = 255           │
        └──────────┬──────────────────┘
                   │  收到 BEACON 且 en2m_consider_parent() 返回 true
                   │  → EN2M_EVENT_PARENT_FOUND
                   │  → en2m_model_on_link_change(true)
                   │     → next_report_ms = now + 200
                   ▼
        ┌─────────────────────────────┐
        │  HAS_PARENT                 │
        │  has_parent = true          │
        │  path_cost  = 父 cost + 1   │
        │  parent_last_us = 最后确认  │
        └──────────┬──────────────────┘
                   │  维护 tick 发现
                   │  now - parent_last_us > EN2M_PARENT_STALE_MS (20 s)
                   │  → 日志 "parent stale"
                   │  → EN2M_EVENT_PARENT_LOST
                   │  → en2m_model_on_link_change(false)
                   ▼
              回到 NO_PARENT
```

### 选父判定（`en2m_consider_parent`）

收到每一个 BEACON 都会跑一遍，按顺序：

```
1. 自己是 COORDINATOR            → 不选父，false
2. 发送方角色不是 COORD/ROUTER   → false（Leaf 不当父）
3. 对方 cost >= 254              → false（对方自己都没入网）
4. new_cost = 对方 cost + 1
   new_cost > EN2M_HOP_LIMIT (8) → false（太深）
5. 判断是否更好：
   ├─ 当前没有父                        → 更好
   ├─ new_cost < 当前 path_cost         → 更好
   ├─ new_cost == path_cost 且就是当前父 → 刷新 parent_last_us，返回 false
   │                                       （这是心跳续命的主路径，不发事件）
   └─ new_cost == path_cost 且是别人     → 只有 rssi > 当前父 rssi + 8
                                           才算更好（8 dB 滞回，防抖动）
6. 更好 → 记下 parent_mac / path_cost / parent_last_us，
          把父加成 ESP-NOW peer，打日志，返回 true
```

### 两个值得注意的行为

**① 父节点 cost 升高时不会续命**。第 5 步只有 `new_cost <= path_cost` 的分支会
刷新 `parent_last_us`。如果父节点自己掉了一层（cost 变大），子节点既不会更新，
也不会续命，于是 20 秒后超时、回到 NO_PARENT、重新选一个更好的父。
这是有意的：宁可短暂断开重选，也不要挂在一条变差的路径上。

**② 续命依赖 beacon，不依赖数据**。`parent_last_us` 只在收到 beacon 时刷新。
默认 beacon 间隔 5 秒、stale 20 秒，也就是**连丢 4 个 beacon 才判定掉线**。

### Leaf / Router / Coordinator 的行为差异

| | Coordinator | Router | Leaf |
|---|---|---|---|
| 选父 | 不选，自己是根 | 选 | 选 |
| 发 beacon | 发，`cost = 0` | 有父才发，`cost = path_cost` | **不发** |
| 转发上行 | — | 转发到父（`en2m_forward_toward_coord`） | 不转发 |
| 转发下行 | 发起 | 按路由表转发（`en2m_forward_toward_dest`） | 不转发 |
| 接受上行 | 接受（受配网/路由约束） | 仅转发 | — |
| 供电 | USB | 常电 | 可电池 |

详见 [mesh.md](mesh.md)。

---

## 7. 下行 ACK / 重传状态机

**仅协调器侧**。每个未确认的下行占一个 `en2m_pending_t` 槽位
（默认 `EN2M_MAX_PENDING = 4`）。

```
en2m_send_downlink(dest, cmd_id, data)
│
├─ esp_now_send 成功 且 cmd_id != 0
│    → en2m_pending_add(dest, cmd_id, pkt)
│        找 (dest, cmd_id) 相同的旧槽位，否则找空槽
│        都没有 → WARN「没有空重传槽，该命令不做确认」，直接放弃跟踪
│        attempts = 1
│        next_us  = now + retry_interval_ms (默认 400 ms)
│
└─ 之后每个 100 ms 维护 tick 跑 en2m_pending_tick()：
     对每个 now >= next_us 的槽位：
       ├─ attempts > max_retries (默认 3) ？
       │    是 → 释放槽位，记入超时列表
       │         → WARN + EN2M_EVENT_ACK_TIMEOUT { mac, transaction_id, attempts }
       └─ 否 → attempts++；next_us = now + interval；按路由重发
```

默认参数下的时间线：

| 时刻 | 动作 | attempts |
|---|---|---|
| 0 ms | 首次发送 | 1 |
| 400 ms | 第 1 次重传 | 2 |
| 800 ms | 第 2 次重传 | 3 |
| 1200 ms | 第 3 次重传 | 4 |
| 1600 ms | 判定超时 | 4 |

**共 4 次发送，约 1.6 秒后放弃**。（`EN2M_CMD_RETRIES = 3` 指的是重传次数。）

任何时刻收到匹配的 ACK：

```
协调器收到 EN2M_MSG_ACK，origin == dest 且 cmd_id 匹配
│
├─ en2m_pending_resolve() → 槽位释放，取出 attempts
└─ EN2M_EVENT_ACK_RECEIVED { mac, transaction_id, attempts }
      → 协调器同时把这一帧作为 uplink 交给 on_uplink，
        于是 USB 上出现 {"type":"ack","id":7,"ok":true}
```

超时那一侧由协调器固件翻译成：

```json
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":false,"error":"timeout"}
```

Bridge 收到 `ok:false` 会打一条 warning。

### `cmd_id = 0` 的语义

`id` 为 0 表示**发了就不管**：不占 pending 槽、不重传、设备也不回 ACK。
协调器固件在 host 没给 `id` 时会用 `en2m_next_cmd_id()` 自动分配一个非 0 值，
所以正常路径总是带确认的。

---

## 8. 持久化生命周期

只有**建的时候声明了 `persist = true`** 的属性会进 NVS。
哪些属性默认就是 persist，见 [persistence.md](persistence.md)。

```
                建模型
                  │
                  ▼
    en2m_attribute_create(..., persist = true)
    或 en2m_cluster_create() 自动带的 persist 默认属性
                  │
    ┌─────────────┴──────────────────────────────────┐
    │            en2m_start()                        │
    │  ① en2m_dm_restore()                           │
    │      nvs_open("en2m_attr", READONLY)            │
    │      逐个 persist 槽位：                        │
    │        key  = "%u_%04x_%04x" (ep, cluster, attr)│
    │        blob = { uint8_t type; int64_t raw; }    │
    │        读到 → coerce(type, raw) 写进槽位        │
    │               persist_dirty = false             │
    │  ② en2m_model_apply_persisted()                 │
    │      逐个 persist 属性调 en2m_attribute_write() │
    │      → write 回调 → **硬件恢复到断电前状态**    │
    └─────────────┬──────────────────────────────────┘
                  │
                  ▼  运行中
    值发生变化 → persist_dirty = persist
                  │
                  ▼
    en2m_model_tick：距上次刷盘 ≥ 5000 ms
      → en2m_dm_flush_persist()
          逐个 persist_dirty 的槽位：
            持锁抄快照，persist_dirty = false，解锁
            懒打开 nvs（只在真的有东西要写时才 open READWRITE）
            nvs_set_blob(key, {type, raw})
          最后有写过 → nvs_commit → nvs_close
                  │
                  ▼
    en2m_stop() → 再刷一次 flush_persist()
```

### 为什么是 5 秒批量刷，不是每次改都写

一个调光灯被 HA 的滑条拖过去，可能在 1 秒内产生几十次 `CURRENT_LEVEL` 变化。
每次都 `nvs_commit` 会很快磨坏 flash。所以脏标记累积，**每 5 秒最多刷一次**，
而且只写真正脏了的槽位。代价是掉电可能丢最近 5 秒的变化。

---

## 9. Identify 倒计时

HA 让设备"闪一下认人"时：

```
收到 IDENTIFY 命令（seconds = 10）
│
├─ en2m_attribute_set(ep, IDENTIFY, IDENTIFY_TIME, u16(10))
├─ EN2M_EVENT_IDENTIFY { endpoint_id, seconds = 10 }
└─ identify 回调(ep, 10, ctx)          ← 第一次调用
      │
      ▼  之后由 en2m_model_identify_tick() 驱动，每秒一次
   remaining = 9 → attribute_set(IDENTIFY_TIME, 9) → identify 回调(ep, 9)
   remaining = 8 → ...
   ...
   remaining = 0 → attribute_set(IDENTIFY_TIME, 0) → identify 回调(ep, 0)
      │
      ▼  IDENTIFY_TIME == 0，倒计时停止
```

倒计时节流用的是 `s_model.next_identify_ms`，所以即使 tick 是 100 ms 一次，
`identify` 回调也是**每秒恰好一次**。回调收到 `seconds == 0` 就该停止效果
（关掉闪灯 / 蜂鸣器）。

只有建了 `EN2M_CLUSTER_IDENTIFY` 的 endpoint 才有倒计时——
设备类型配方**不会**自动加 Identify cluster，需要自己
`en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY)`（`dimmable_light` 示例演示了）。
没建这个 cluster 时，`identify` 回调仍会被调用一次（来自命令那一步），
但不会有倒计时。

---

## 10. 100 ms 维护 tick 里到底做了什么

en2m 任务的主循环：

```c
while (s_ctx.running) {
    if (xQueueReceive(s_ctx.queue, &item, pdMS_TO_TICKS(EN2M_TICK_MS)) == pdTRUE) {
        switch (item.kind) {
        case EN2M_ITEM_RX:   en2m_dispatch_rx(&item);           break;
        case EN2M_ITEM_WORK: item.u.work.fn(item.u.work.arg);   break;
        case EN2M_ITEM_ATTR: en2m_attribute_set(...);           break;
        }
    }
    int64_t now_ms = en2m_now_ms();
    if (now_ms - s_ctx.last_tick_ms >= EN2M_TICK_MS) {
        s_ctx.last_tick_ms = now_ms;
        en2m_maintenance(now_ms);
        en2m_model_tick(now_ms);
    }
}
```

队列等待的超时就是 tick 周期，所以**空闲时任务每 100 ms 醒一次**，
忙的时候靠队列事件驱动、tick 顺带跑。tick 不会因为队列忙而被跳过，
但也不保证精确 100 ms（前一个回调阻塞了就会晚）。

### `en2m_maintenance(now_ms)` — 传输层

持锁部分：

| 动作 | 条件 |
|---|---|
| `en2m_expire_parent` | 非协调器、有父、`now - parent_last_us > 20 s` |
| `en2m_expire_routes` | 路由 `> EN2M_ROUTE_STALE_MS` (120 s)；邻居 `> 2 × PARENT_STALE_MS` (40 s) |
| `en2m_pending_tick` | 见第 7 节 |
| 判定 beacon 到期 | 非 Leaf 且 `now - last_beacon >= EN2M_BEACON_INTERVAL_MS` (5 s) |
| 判定 hello 到期 | 非协调器且 `now - last_hello >= EN2M_HEARTBEAT_MS` (30 s) |
| 抄下 `rx_dropped` | |
| `en2m_send_beacon()` | beacon 到期（在锁内发，因为要读 parent/cost） |

解锁后：

| 动作 |
|---|
| hello 到期 → `en2m_send_uplink(EN2M_MSG_HEARTBEAT, ...)` |
| 父丢失 → `EN2M_EVENT_PARENT_LOST` + `en2m_model_on_link_change(false)` |
| 每条超时 → WARN + `EN2M_EVENT_ACK_TIMEOUT` |
| `rx_dropped != 0` → 清零 + WARN + `EN2M_EVENT_RX_DROPPED` |

### `en2m_model_tick(now_ms)` — 交互层

```
!started → 立即 return
│
├─ en2m_model_identify_tick(now_ms)        每秒一次的 identify 倒计时
│
├─ now - last_persist_ms >= 5000
│    → last_persist_ms = now；en2m_dm_flush_persist()
│
├─ periodic_due = 模式是 DEFAULT 或 PERIODIC_ONLY
│                 且 now - last_report_ms >= report_interval_ms
│
└─ periodic_due 或 (next_report_ms != 0 且 now >= next_report_ms)
     → en2m_dm_refresh()        跑所有 read 回调
     → en2m_report_transmit()   组装 + 发送
```

---

## 11. 停止时序

```
en2m_stop()                                                        [app]
│
├─ !started → ESP_ERR_INVALID_STATE
├─ started = false            ← 先置，让 tick 立刻停止上报
├─ en2m_dm_flush_persist()    最后一次刷盘，不丢最近的变化
├─ en2m_mesh_deinit()
│    ├─ running = false
│    ├─ 最多等 20 × 50 ms，等 en2m 任务自己退出并 vTaskDelete(NULL)
│    ├─ esp_now_deinit() → esp_wifi_stop() → esp_wifi_deinit()
│    └─ 删队列、删互斥锁，inited = false
└─ 发 EN2M_EVENT_STOPPED
```

**数据模型不会被拆掉**：endpoint / cluster / attribute 和它们的当前值都还在，
所以 `en2m_stop()` 之后可以再 `en2m_start()`（比如切信道、改角色），
不用重建模型。但 `s_model.configured` 也还在，配置会被新的覆盖。

---

## 相关文档

- 回调的完整契约与返回值语义 → [callbacks.md](callbacks.md)
- 哪个函数能在哪个上下文调 → [concurrency.md](concurrency.md)
- 上报模式、限流和 JSON 键映射 → [reporting.md](reporting.md)
- 事件 payload → [events.md](events.md)
- 帧格式逐字段 → [mesh.md](mesh.md)
- 各个超时的调参 → [kconfig.md](kconfig.md)

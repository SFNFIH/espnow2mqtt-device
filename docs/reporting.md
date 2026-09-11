# 上报

上报是设备把属性存储序列化成 JSON、发给协调器的过程。
这篇讲**什么时候发、发多频、发什么、太长了怎么办**。

- [1. 什么会触发上报](#1-什么会触发上报)
- [2. 四种上报模式](#2-四种上报模式)
- [3. 周期与限流](#3-周期与限流)
- [4. 上报调度的合并](#4-上报调度的合并)
- [5. 序列化：cluster → HA JSON 键](#5-序列化cluster--ha-json-键)
- [6. `caps` 是怎么算出来的](#6-caps-是怎么算出来的)
- [7. 160 字节与降级策略](#7-160-字节与降级策略)
- [8. 手动控制上报](#8-手动控制上报)
- [9. 调参建议](#9-调参建议)

---

## 1. 什么会触发上报

| 触发 | 机制 | 受 `min_report_interval_ms` 约束？ |
|---|---|---|
| 属性变化 | `on_change` → `en2m_report_schedule(0)` | ✓ |
| 周期到期 | `en2m_model_tick` 判 `now - last_report >= report_interval_ms` | ✗ 周期本身就是节流 |
| 收到命令 | 回 ACK 后 `en2m_report_schedule(0)` | ✓ |
| 选上父节点 | `on_link_change(true)` → `next_report_ms = now + 200` | ✗ 直接改，不过下限 |
| `en2m_start` | `next_report_ms = now + 500` | ✗ |
| `en2m_report_now()` | 在 en2m 任务上 → 立即；其他任务 → 降级成 `schedule(0)` | 取决于上下文 |
| `en2m_report_schedule(ms)` | 显式安排 | ✓ |

注意"选上父节点"和"启动"这两条是**直接写 `next_report_ms`**，绕过了
`en2m_report_schedule` 的下限检查。这是有意的：入网后的第一份状态必须尽快出去，
不能被限流拖住。

---

## 2. 四种上报模式

```c
typedef enum {
    EN2M_REPORT_DEFAULT = 0,    /* 变化时上报（限流）+ 周期保活 */
    EN2M_REPORT_PERIODIC_ONLY,  /* 只按周期 */
    EN2M_REPORT_ON_CHANGE_ONLY, /* 只在变化时 */
    EN2M_REPORT_MANUAL,         /* 只有应用调 en2m_report_now 时 */
} en2m_report_mode_t;
```

| 模式 | 变化触发 | 周期触发 | 适合 |
|---|:-:|:-:|---|
| `DEFAULT` | ✓ | ✓ | **绝大多数设备**。变化立刻可见，周期上报兼作保活 |
| `PERIODIC_ONLY` | ✗ | ✓ | 读数一直在微动的模拟量传感器，只关心趋势 |
| `ON_CHANGE_ONLY` | ✓ | ✗ | 极省电的电池设备；但协调器会因为长时间没消息判它离线 |
| `MANUAL` | ✗ | ✗ | 完全自己控制节奏（比如只在深睡唤醒时发一次） |

### 注意 `ON_CHANGE_ONLY` 和离线判定的冲突

协调器固件在 `EN2M_OFFLINE_MS`（默认 90 秒）没收到任何东西时会把设备标成
`offline`。但心跳（`EN2M_MSG_HEARTBEAT`）是由**传输层**每
`EN2M_HEARTBEAT_MS`（默认 30 秒）自动发的，和上报模式无关，
所以 `ON_CHANGE_ONLY` 的设备**也不会被误判离线**。

心跳帧不带 payload，只刷新协调器的 last-seen 和路由表。

---

## 3. 周期与限流

两个参数，作用完全不同：

```c
uint32_t report_interval_ms;     /* 周期保活间隔。0 = 按角色取默认 */
uint32_t min_report_interval_ms; /* 变化上报的最小间隔。0 = 1000 ms */
```

### `report_interval_ms` — 多久发一次保活

`0` 时按角色取默认：

| 角色 | 默认 | 常量 |
|---|---|---|
| `EN2M_ROLE_LEAF` | 30000 ms | `EN2M_REPORT_INTERVAL_LEAF_MS` |
| `ROUTER` / `COORDINATOR` | 15000 ms | `EN2M_REPORT_INTERVAL_MAINS_MS` |

Leaf 默认更慢是因为它可能是电池供电。

它衡量的是**距上次任何一次上报**的时间，不是距上次周期上报。
所以一个变化频繁的设备实际上很少走到周期分支——变化上报已经把
`last_report_ms` 刷新了。

### `min_report_interval_ms` — 变化上报的地板

`0` 时取 `EN2M_MIN_REPORT_INTERVAL_MS` = 1000 ms。

它是变化触发的**下限**：

```c
floor_ms = last_report_ms + min_report_interval_ms;
when     = now + delay_ms;
if (when < floor_ms) {
    when = floor_ms;          /* 往后推到地板 */
}
```

也就是说无论属性变多快，**变化上报最快也是每 `min_report_interval_ms` 一次**。
中间的变化不会丢，因为上报读的是属性存储的**当前值**，
最后那次变化一定会被下一份报文带出去。

举例，`min_report_interval_ms = 1000`，HA 拖亮度滑条：

```
t=0     brightness 10  → schedule → floor = 0+1000 → next = 1000
t=100   brightness 40  → schedule → floor = 1000   → next 已是 1000，不变
t=200   brightness 90  → ...
t=900   brightness 200 → ...
t=1000  上报，带 brightness = 200      ← 只发一次，值是最终值
```

**这就是限流的正确形态**：不是丢采样，而是合并成一次上报。

### 去重和限流的区别

| | 去重 | 限流 |
|---|---|---|
| 在哪 | 属性存储 (`en2m_attribute_set`) | 上报调度 (`en2m_report_schedule`) |
| 做什么 | 值没变就不产生任何通知 | 值变了但上报推迟到地板 |
| 关掉？ | 不能 | 把 `min_report_interval_ms` 设成 1 |

---

## 4. 上报调度的合并

`next_report_ms` 是**单个**时间戳，`0` 表示没有安排。
`en2m_report_schedule` 的合并规则是**最早的赢**：

```c
if (s_model.next_report_ms == 0 || when < s_model.next_report_ms) {
    s_model.next_report_ms = when;
}
```

所以：

- 连续 10 次 `en2m_report_schedule(0)` 只会产生 1 份报文
- `en2m_report_schedule(5000)` 之后再 `en2m_report_schedule(0)`，
  实际在地板时刻发（更早的赢）
- `en2m_report_schedule(0)` 之后再 `en2m_report_schedule(5000)`，
  还是在地板时刻发（已安排的更早）

报文发出后 `next_report_ms` 归零，等下一次触发。

这两个 `int64_t` 由一个 `portMUX_TYPE` 自旋锁保护，
因为 `en2m_report_schedule` 可以从任意任务被间接调到
（见 [concurrency.md](concurrency.md#3-两把锁)）。

---

## 5. 序列化：cluster → HA JSON 键

上报的 JSON 用的是**扁平的、Home Assistant 风格的键**，不是嵌套的 cluster 结构。
这样 Bridge 和 HA 集成都不用理解 Matter 的 ID。

`en2m_report_cluster()` 的完整映射：

| Cluster | Attribute | JSON 键 | 值 | 换算 | 顺带加的 `caps` |
|---|---|---|---|---|---|
| OnOff | `ON_OFF` | `switch` | `"ON"` / `"OFF"` | | `switch` |
| LevelControl | `CURRENT_LEVEL` | `brightness` | 数字 | 原样 0–254 | `light` |
| ColorControl | `COLOR_TEMPERATURE_MIREDS` | `color_temp` | 数字 | 原样 mired | `light` |
| | | `color_mode` | `"color_temp"` | 固定值 | |
| Switch | `PRESS_COUNT` | `button` | 数字 | 原样，自增 | `button` |
| | `PRESS_ACTION` | `button_action` | `"press"`/`"double_press"`/… | 枚举转字符串 | |
| BooleanState | `STATE_VALUE` | `contact` | `"ON"` / `"OFF"` | | `contact` |
| Occupancy | `OCCUPANCY` | `occupancy` | `"ON"` / `"OFF"` | | `occupancy` |
| Illuminance | `MEASURED_VALUE` | `illuminance` | 数字 | 原样 lux | `illuminance` |
| TemperatureMeasurement | `MEASURED_VALUE` | `temperature` | 数字 | **÷ 100** → °C | `temperature` |
| RelativeHumidity | `MEASURED_VALUE` | `humidity` | 数字 | **÷ 100** → % | `humidity` |
| PressureMeasurement | `MEASURED_VALUE` | `pressure` | 数字 | **÷ 10** → hPa | `pressure` |
| SmokeCO | `SMOKE_STATE` | `smoke` | `"ON"` / `"OFF"` | | `smoke` |
| | `CO_STATE` | `carbon_monoxide` | `"ON"` / `"OFF"` | | `carbon_monoxide` |
| ElectricalPower | `ACTIVE_POWER_MW` | `power` | 数字 | **÷ 1000** → W | `power` |
| | `ENERGY_MWH` | `energy` | 数字 | **÷ 1000** → Wh | `energy` |
| FanControl | `FAN_MODE` | `fan_mode` | `"off"`/`"low"`/… | 枚举转字符串 | `fan` |
| | `PERCENT_SETTING` | `percentage` | 数字 0–100 | 原样 | |
| WindowCovering | `CURRENT_POSITION_LIFT_PERCENT` | `position` | 数字 0–100 | 原样 | `cover` |
| | | `cover` | `"CLOSED"`（≥ 95）/ `"OPEN"` | 由位置推导 | |
| DoorLock | `LOCK_STATE` | `lock` | `"LOCKED"` / `"UNLOCKED"` | | `lock` |
| Thermostat | `SYSTEM_MODE` | `hvac_mode` | `"off"`/`"cool"`/… | 枚举转字符串 | `climate` |
| | `LOCAL_TEMPERATURE` | `current_temperature` | 数字 | **÷ 100** → °C | |
| | 按当前模式挑 setpoint | `target_temperature` | 数字 | **÷ 100** → °C | |

### 按键报的是计数器，不是"按了"

```c
if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_PRESS_COUNT, &v) && v > 0) {
    cJSON_AddNumberToObject(root, "button", (double)v);
    en2m_caps_add(caps, "button");
    if (en2m_model_read(ep_id, cluster_id, EN2M_ATTR_PRESS_ACTION, &v2)) {
        cJSON_AddStringToObject(root, "button_action", en2m_press_action_str((uint8_t)v2));
    }
}
```

**每一条上报都是完整快照，而 `<slug>/state` 是 retained 的。**
所以"按了一下"这件事只能通过某个值**变了**来表达。
`button_action` 单独用不行：连续两次短按会产生两条一模一样的 payload，
和"同一条被重发"完全无法区分。计数器解决这个问题。

应用侧不需要管计数器，`en2m_report_button()` 自己加：

```c
en2m_report_button(ENDPOINT, EN2M_PRESS_DOUBLE);
```

`&& v > 0` 那个条件也是必要的：一个从没被按过的节点**不报 `button`**，
否则 HA 会在收到第一条 retained 报文时凭空造出一次按键事件。

计数器是持久化的，见
[persistence.md](persistence.md)。两次按键间隔小于
`min_report_interval_ms` 时会被合并成一条上报——计数器仍然加了两次，
所以接收方能从跨度看出漏了几次。

### Identify cluster 不上报

`IDENTIFY_TIME` 没有对应的 HA 键，所以序列化时会被跳过（走 `default: break`）。
Identify 是个纯本地效果。

### 窗帘的 `cover` 键是推导出来的

```c
cJSON_AddStringToObject(root, "cover", v >= 95 ? "CLOSED" : "OPEN");
```

95 这个阈值是为了容忍电机的机械误差——关到 96% 就该算关好了。
HA 集成同时收到 `position` 和 `cover`，用 `position` 做滑条、用 `cover` 做状态。

### 温控器只报"当前在追的那个" setpoint

```c
en2m_model_read(ep_id, cluster_id,
                (v == EN2M_THERMOSTAT_COOL) ? EN2M_ATTR_OCCUPIED_COOLING_SETPOINT
                                            : EN2M_ATTR_OCCUPIED_HEATING_SETPOINT,
                &v2);
```

两个 setpoint 都存着，但只报一个，键固定叫 `target_temperature`。
HA 的 `climate` 实体在单 setpoint 模式下就是这个语义，
而且省下了一个字段的空间（见第 7 节）。

### 扁平键是全局的

键里**不带 endpoint 编号**。序列化时同一个 cluster 只处理一次
（`seen_ids[]` 去重），编号最小的 endpoint 赢。多 endpoint 的限制见
[data-model.md](data-model.md#什么时候需要多个-endpoint)。

---

## 6. `caps` 是怎么算出来的

`caps` 是一个字符串数组，告诉 HA 集成"这个设备该建哪些实体"。
它**不是**配置出来的，而是**从实际存在的 cluster 推出来的**——
每个 cluster 序列化成功时顺带 `en2m_caps_add()` 一个名字（见上表最后一列）。

### 一条修正规则

```c
/* 有亮度或色温的节点在 HA 里是 light，不是 switch */
if (brightness 存在 || color_temp 存在) {
    从 caps 里删掉 "switch";
}
```

因为一个调光灯同时有 OnOff 和 LevelControl，两个 cluster 会分别加
`switch` 和 `light`。但在 HA 里它应该是**一个** `light` 实体，
而不是一个 `light` 加一个 `switch`。所以有 `light` 时把 `switch` 删掉。

### 全部可能的 caps

`switch`、`light`、`button`、`contact`、`occupancy`、`illuminance`、
`temperature`、`humidity`、`pressure`、`smoke`、`carbon_monoxide`、
`power`、`energy`、`fan`、`cover`、`lock`、`climate`。

### `caps` 缺失时会怎样

HA 集成在 payload 里**没有** `caps` 时会**从键名反推**能力。
所以上报降级掉 `caps` 之后功能不受影响，只是第一份报文如果恰好赶上降级、
实体创建可能晚一轮。

---

## 7. 160 字节与降级策略

一个 ESP-NOW 帧的载荷上限是 `EN2M_DATA_MAX = 160` 字节
（`en2m_pkt_t` 整体必须 ≤ 250，有 `_Static_assert` 兜着）。

上一版代码超长时直接 `memcpy` 前 160 字节，于是发出**非法 JSON**，
HA 那边解析失败、状态卡住。现在按三档构建，发第一个装得下的：

| 档 | `node_role` | `caps` | 值 |
|---|:-:|:-:|:-:|
| 1 | ✓ | ✓ | ✓ |
| 2 | | ✓ | ✓ |
| 3 | | | ✓ |

```c
static const struct { bool caps; bool diagnostics; } levels[] = {
    {true, true}, {true, false}, {false, false}
};
for (size_t i = 0; i < 3; i++) {
    json = en2m_report_build(levels[i].caps, levels[i].diagnostics);
    if (strlen(json) <= EN2M_DATA_MAX) break;
    cJSON_free(json);  json = NULL;
}
```

### 为什么降级是安全的

因为 **host 端会合并同一设备的连续上报**。HA 集成里：

```python
merged = dict(dev.state)
merged.update(payload)
```

所以某一份报文省掉 `node_role` 或 `caps`，之前收到的那份里的值还在。
**降级不丢信息，只丢这一帧里的冗余。**

这也是为什么早期版本里那个 `clusters` 数组被整个删掉了——
Bridge 和 HA 集成都不消费它，纯占字节。

### 三档都装不下

```c
json = en2m_report_build(false, false);
len = EN2M_DATA_MAX;                /* 真的截断 */
event.truncated = true;
ESP_LOGE(TAG, "report exceeds %d bytes; split the device across endpoints or trim clusters", 160);
```

这是**唯一**会发出非法 JSON 的路径，而且会打 `ESP_LOGE` 并把
`EN2M_EVENT_REPORT_SENT.truncated` 置位。正常设备碰不到——

粗略估算，纯值部分每个键约 15–25 字节：

| 设备 | 键 | 大概字节 |
|---|---|---|
| 开关 | `switch` | ~20 |
| 色温灯 | `switch` `brightness` `color_temp` `color_mode` | ~85 |
| 温湿气压三合一 | `temperature` `humidity` `pressure` | ~75 |
| 温控器 | `hvac_mode` `current_temperature` `target_temperature` | ~90 |
| 计量插座 | `switch` `power` `energy` | ~65 |

加上 `caps` 数组（每项约 `"temperature",` 14 字节）和 `node_role`（约 22 字节），
装 4–6 个 cluster 都还有余量。要塞 8 个 cluster 就得拆到两个设备了。

### 怎么知道自己被截断了

```c
static void on_en2m(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == EN2M_EVENT_REPORT_SENT) {
        const en2m_event_report_t *r = data;
        if (r->truncated) {
            ESP_LOGE(TAG, "报文被截断了，得拆 cluster");
        }
        if (r->err != ESP_OK) {
            ESP_LOGW(TAG, "上报没发出去: %s", esp_err_to_name(r->err));
        }
    }
}
```

`err` 常见值：

| `err` | 原因 |
|---|---|
| `ESP_OK` | 发出去了（ESP-NOW 层面） |
| `ESP_ERR_INVALID_STATE` | **还没有父节点**，STATE 帧不广播 |
| `ESP_ERR_NOT_SUPPORTED` | 角色是 COORDINATOR，不该上报 |
| `ESP_FAIL` | 加 peer 失败 |

---

## 8. 手动控制上报

### 立即上报

```c
en2m_report_now();
```

**但要注意上下文**：

| 从哪调 | 实际行为 |
|---|---|
| en2m 任务（回调里） | 立即 `dm_refresh()` + `report_transmit()`，**不受限流约束** |
| 其他任务 | 降级成 `en2m_report_schedule(0)`，**受限流约束** |

想从别的任务真正立刻发，用 `en2m_schedule` 绕一下：

```c
static void do_report(void *arg) { en2m_report_now(); }
/* 别的任务里： */
en2m_schedule(do_report, NULL);
```

这么绕的原因是 read 回调和 cJSON 序列化必须留在 en2m 任务上——
否则一个应用任务就能在任意时刻调你的 I2C 驱动，和 en2m 任务撞车。

### 延迟上报

```c
en2m_report_schedule(2000);     /* 2 秒后（不早于地板） */
en2m_report_schedule(0);        /* 尽快（不早于地板） */
```

典型用法是一批相关属性都写完再上报一次：

```c
en2m_attribute_set(1, TEMP, MEASURED, en2m_i16(t));
en2m_attribute_set(1, HUMIDITY, MEASURED, en2m_u16(h));
en2m_attribute_set(1, PRESSURE, MEASURED, en2m_i32(p));
/* 三次 on_change 各安排了一次上报，合并成一份，天然就对 */
```

其实不用手动管——合并逻辑已经保证了这一点。

### 完全接管

```c
en2m_device_config_t cfg = {
    .report_mode = EN2M_REPORT_MANUAL,
    /* ... */
};
```

然后自己决定什么时候 `en2m_report_now()`。注意心跳还是会自动发，
所以设备不会被判离线。

---

## 9. 调参建议

| 场景 | `report_mode` | `report_interval_ms` | `min_report_interval_ms` |
|---|---|---|---|
| 开关 / 灯 / 锁（执行器） | `DEFAULT` | 0（默认 30 s） | 0（默认 1 s） |
| 温湿度（DHT22，2 s 硬限制） | `DEFAULT` | 60000 | 5000 |
| 温湿度（I2C，可快读） | `DEFAULT` | 30000 | 2000 |
| 门磁 / PIR（要求低延迟） | `DEFAULT` | 0 | **200** |
| 计量插座（功率一直在动） | `DEFAULT` | 15000 | 5000 |
| 窗帘（行程中连续上报） | `DEFAULT` | 0 | **200** |
| 电池设备，极省电 | `ON_CHANGE_ONLY` | — | 2000 |
| 抖动很大的模拟量 | `PERIODIC_ONLY` | 60000 | — |

### 几条经验

**`min_report_interval_ms` 调小要看清代价。** 200 ms 意味着最坏情况每秒 5 帧。
ESP-NOW 单帧 250 字节，5 帧/秒对一个节点没问题，但 20 个节点都这么干
就会开始撞包。门磁这类"变化很少但要求快"的设备适合调小，
功率计这类"一直在变"的不适合。

**`report_interval_ms` 不用调很短。** 它只是保活。真正的状态更新靠变化触发。
调短只会白耗电、白占空口。

**拉式传感器的采样频率 = 上报频率。** `report_interval_ms = 60000` 就意味着
`attribute_read` 每 60 秒被调一次。想采得更密但报得更疏，就自己建
`esp_timer` 采样并 `en2m_attribute_set`，然后用 `PERIODIC_ONLY`。

---

## 相关文档

- 上报流水线的每一步 → [state-flow.md](state-flow.md#3-传感器上报流水线)
- `attribute_read` 的完整契约 → [callbacks.md](callbacks.md#2-attribute_read--采一个新值)
- 单位换算和属性类型 → [data-model.md](data-model.md#单位约定)
- 空中协议里 payload 长什么样 → [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md)
- `EN2M_EVENT_REPORT_SENT` 的 payload → [events.md](events.md)

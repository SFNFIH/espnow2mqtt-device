# `occupancy_sensor` — 人在传感器 + 照度

**推、拉两种传感器共存在一个 endpoint 上的示例。**
人在状态是事件来的（PIR 一有动作就必须马上报），
照度是数值型的（随时能读，跟着上报周期走就行）。
这两种节奏放在同一个设备里，各走自己该走的路。

| | |
|---|---|
| Cluster | Occupancy (`0x0406`) + Illuminance (`0x0400`) |
| 设备类型 | `OCCUPANCY_SENSOR` + `LIGHT_SENSOR`（叠在同一个 endpoint 上） |
| 回调 | `attribute_read`（照度 + 人在兜底）+ `en2m_schedule` 推送（人在） |
| 外设组件 | [`espressif/bh1750`](https://components.espressif.com/components/espressif/bh1750) `^2.0.0` + [`espressif/button`](https://components.espressif.com/components/espressif/button) `^4.2.1` |
| 默认名 / slug | `occ1` |
| HA 实体 | `binary_sensor.occ1_occupancy` + `sensor.occ1_illuminance` |

---

## 接线

| GPIO | 接什么 | 说明 |
|---|---|---|
| **4** | BH1750 `SDA` | 打开了内部上拉 |
| **5** | BH1750 `SCL` | 同上 |
| **6** | PIR 的 `OUT` | **高电平有效**，不加内部上拉 |
| 3V3 | BH1750 `VCC` | |
| 5 V | PIR `VCC` | HC-SR501 要 5 V；AM312 是 3.3 V 的 |
| GND | 两个都要共地 | |

BH1750 的 `ADDR` 脚悬空或接地是 `0x23`，接 3V3 是 `0x5C`。
代码里用 `BH1750_I2C_ADDRESS_DEFAULT`（`0x23`）。

PIR 模块的输出是**推挽**的（自己驱动高低电平），
所以 `button_gpio_config_t` 里设了 `.disable_pull = true` ——
内部上拉会和模块的输出级对抗。

三个引脚都在 `main/main.c` 顶部的 `#define` 里。
换引脚前先看 [docs/wiring.md](../../docs/wiring.md#esp32-c3-引脚选择须知)。

**没接 BH1750 的话固件起不来**：`sensor_init()` 里
`bh1750_create()` 在 I²C 上找不到器件会返回 `ESP_ERR_NOT_FOUND`，
被 `ESP_ERROR_CHECK` 拦下。想先只验证 PIR 就把
`ESP_ERROR_CHECK(light_sensor_init())` 换成
`ESP_ERROR_CHECK_WITHOUT_ABORT(...)`，照度那一路会一直返回错误、
在上报里缺字段，其它照跑。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/occupancy_sensor
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

第一次 `build` 会联网把 `espressif/bh1750` 和 `espressif/button`
下载到本工程的 `managed_components/`。

---

## 跑起来应该看到什么

```
espnow2mqtt/occ1/availability online
espnow2mqtt/occ1/state          {"occupancy":"OFF","illuminance":137,
                                 "caps":["occupancy","illuminance"],"hop":1}
```

在 PIR 前面挥一下手，**几十毫秒内**就有一条 `"occupancy":"ON"` 出来；
模块自己的保持时间（SR501 上那个电位器，几秒到几分钟）到了之后再来一条 `"OFF"`。

`illuminance` 单位 **lux**，是整数（HA 那边显示精度 0 位），
跟着周期上报走，不会因为光线变化自己发。

---

## 两条路各走各的

```
人在（推）：PIR 边沿回调 → en2m_schedule(publish_motion)
                        → en2m_report_occupancy() → 立刻上报

照度（拉）：上报定时器到 → on_read(ILLUMINANCE) → 读 BH1750 → 进这一轮上报
```

为什么不反过来？

**人在状态不能用拉。** PIR 的输出是一个几秒钟的脉冲，
你在下一个上报周期去读它，脉冲早过去了，等于什么都没检测到。
必须在事件发生的那一刻推出去。

**照度不该用推。** 光照是连续变化的，每变一点就发一次的话
射频会被刷爆（云飘过去就能刷出几十条）。数值型的量跟着上报周期走才对。

这条规律对所有传感器都成立：**看输出是脉冲还是电平**。
脉冲（PIR、雨滴计数、门铃）用推，电平（温度、光照、电压）用拉。

### PIR 为什么用按键组件

一个 PIR 模块的输出就是一根会变电平的线：有人时拉高，保持时间到了拉低。
这和一个按键没有任何区别，所以这里直接用
[`espressif/button`](https://components.espressif.com/components/espressif/button)，
把两个边沿都注册上：

```c
iot_button_register_cb(s_pir, BUTTON_PRESS_DOWN, NULL, on_pir_edge, NULL);
iot_button_register_cb(s_pir, BUTTON_PRESS_UP,   NULL, on_pir_edge, NULL);
```

`PRESS_DOWN` 是"有人了"，`PRESS_UP` 是"保持时间过了"。
**两个都要注册**——只挂一个的话你永远报不出另一边。
这是自己写 GPIO 中断时最常犯的错（只配上升沿，于是只报 `ON` 不报 `OFF`），
用组件的话两行摆在一起，很难写漏。

回调里回读电平而不是靠记状态，理由和
[`contact_sensor`](../contact_sensor/README.md#回调里为什么要回读硬件) 一样。

### `en2m_schedule` 和 `en2m_schedule_from_isr` 的区别

```c
static void on_pir_edge(void *button_handle, void *usr_data)
{
    en2m_schedule(publish_motion, NULL);    // ← 不带 _from_isr
}
```

这里用的是 `en2m_schedule()`，因为 `espressif/button` 不用 GPIO 中断——
它在一个 esp_timer 上轮询加消抖，回调跑在**普通任务**上。

- 从**任务**里调 → `en2m_schedule()`
- 从**中断**里调 → `en2m_schedule_from_isr()` + `portYIELD_FROM_ISR()`

**那既然不是中断，为什么不在回调里直接调 `en2m_report_occupancy()`？**
技术上可以。但那个 esp_timer 是**全固件所有按键共用的一个**，
在回调里取 en2m 的锁、等发包，会连带卡住同一块板上其它按键的消抖。
而且 `en2m_schedule` 把所有数据模型操作收拢到一个任务上，
省掉了一整类竞态问题。这个库里所有"改状态"的操作都应该走 en2m 任务，
见 [docs/concurrency.md](../../docs/concurrency.md)。

### 顺序：`en2m_start` 之后才启动事件源

```c
ESP_ERROR_CHECK(light_sensor_init());   // ← I²C 在前
...
ESP_ERROR_CHECK(en2m_start(&cfg));
ESP_ERROR_CHECK(pir_init());            // ← 事件源在后
```

`pir_init()` 反过来写的话，PIR 可能在 `en2m_start()` 建好队列之前就触发，
那次 `en2m_schedule()` 会直接返回 `ESP_ERR_INVALID_STATE`——第一次动作就丢了。
（不会崩，但会丢。）

**通用规则：事件源（中断、定时器、按键）一律在 `en2m_start()` 之后启动，
硬件初始化（I²C、GPIO 输出）一律在之前。**
后者是因为持久化状态的恢复会在 `en2m_start()` 里调你的写回调，
那时候硬件必须已经能用了。

这个示例两边都有：`light_sensor_init()` 在前，`pir_init()` 在后。

---

## 换成别的传感器

### 毫米波：LD2410 / LD2450

比 PIR 好得多——能检测**静止的人**（PIR 只对移动敏感，人坐着不动就报"没人"）。
接口是 UART，注册表里没有对应组件，需要自己写一个解析任务：

```c
/* 解析任务里，拿到一帧之后： */
if (presence != s_last_presence) {
    s_last_presence = presence;
    en2m_schedule(publish_presence, NULL);   // 从任务里调，不带 _from_isr
}
```

注意这时 `on_read(OCCUPANCY)` 的兜底路径要改成返回 `s_last_presence`，
不能再去读 GPIO 了。

### 照度：换成 TSL2591 / VEML7700

BH1750 量程 1–65535 lux，够室内用。要测更暗（月光级）或者更亮（直射阳光）
就得换更宽量程的器件。
注册表里有 [`espressif/veml6040`](https://components.espressif.com/components/espressif/veml6040)
和 `veml6075`（后者是紫外）。

### 为什么 BH1750 跑连续模式

```c
bh1750_power_on(s_bh1750);
bh1750_set_measure_mode(s_bh1750, BH1750_CONTINUE_1LX_RES);
```

BH1750 的**单次**高分辨率测量要 120 ms，
如果在 `on_read()` 里"触发测量 + 等结果"，就会把 en2m 任务卡住 120 ms，
期间收不了包。

**连续模式**让器件自己一直测，结果一直放在寄存器里，
`on_read()` 里的 `bh1750_get_data()` 只是一次几毫秒的 I²C 读。
代价是常电流从几 µA 涨到 120 µA —— 对常电设备完全无所谓。

电池设备要反过来：用 `BH1750_ONETIME_1LX_RES`，
但那 120 ms 就不能放在 `on_read()` 里了，得起一个 `esp_timer` 周期采样，
`on_read()` 只返回缓存。
（另一种思路见 [`th_sensor`](../th_sensor/README.md#一次测量供两个属性用) 的时间戳缓存。）

---

## 人在 vs 移动

HA 里有两个不同的 device class：

| `caps` 里的 | HA device class | 语义 |
|---|---|---|
| `occupancy` | `occupancy` | **有人在**（可以是静止的） |
| `motion` | `motion` | **有动作** |

PIR 严格来说测的是 `motion`；毫米波测的是 `occupancy`。
这个示例用 `occupancy`，因为多数人做的是"房间有人就开灯"的自动化。

要改成 `motion` 的话，序列化那边的 key 要变——
改法见 [docs/data-model.md](../../docs/data-model.md)。

**做自动化的时候注意**：PIR 有几秒到几分钟的保持时间，
所以"没人了就关灯"会在最后一次动作之后延迟触发。
HA 里再加一层 `for: minutes: 5` 的延迟通常比调 PIR 的电位器省事。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 启动就挂在 `light_sensor_init()` | I²C 上找不到 0x23。查接线、上拉，确认 `ADDR` 脚没接 3V3 |
| 只报 `ON` 从不报 `OFF` | 只注册了 `BUTTON_PRESS_DOWN`。两个边沿都要 |
| PIR 一直报有人 | 模块对着热源（暖气、显示器、阳光直射）；或者 5 V 供电不稳 |
| PIR 完全没反应 | `PIR_ACTIVE_LEVEL` 反了，或者忘了 `.disable_pull = true` |
| 上电头一分钟 PIR 乱跳 | HC-SR501 有 30–60 秒的预热期，正常 |
| 照度一直是 0 | 连续模式没设上，或者传感器被挡住了 |
| 上线后头几秒 HA 里"不可用" | 正常，第一条上报之前 HA 不知道有哪些 cap |

### 人在状态的自愈路径

`on_read()` 里除了照度还处理了 `EN2M_CLUSTER_OCCUPANCY`：

```c
case EN2M_CLUSTER_OCCUPANCY:
    *out_value = en2m_bool(pir_is_active());
    return ESP_OK;
```

这一段是**兜底**，和 [`contact_sensor`](../contact_sensor) 里的路径 B 一个作用：
ESP-NOW 是无确认传输，一次丢包会让 HA 里的状态和现实不一致，
有了这一段，下一个周期上报就会自动对齐，不用等下一次有人经过。

---

## 延伸阅读

- [docs/examples.md#occupancy_sensor](../../docs/examples.md#occupancy_sensor) — 逐行精讲
- [docs/concurrency.md](../../docs/concurrency.md) — `en2m_schedule` 家族和任务模型
- [docs/reporting.md](../../docs/reporting.md) — 推送上报和周期上报的关系

# `occupancy_sensor` — 人在传感器 + 照度

**推、拉两种传感器共存在一个 endpoint 上的示例。**
人在状态是事件来的（PIR 一有动作就必须马上报），
照度是数值型的（随时能读，跟着上报周期走就行）。
这两种节奏放在同一个设备里，各走自己该走的路。

| | |
|---|---|
| Cluster | Occupancy (`0x0406`) + Illuminance (`0x0400`) |
| 设备类型 | `OCCUPANCY_SENSOR` + `LIGHT_SENSOR`（叠在同一个 endpoint 上） |
| 回调 | `attribute_read`（照度）+ `en2m_schedule` 推送（人在） |
| 驱动 | 无（`esp_timer` 模拟 PIR，照度是 stub） |
| 默认名 / slug | `occ1` |
| HA 实体 | `binary_sensor.occ1_occupancy` + `sensor.occ1_illuminance` |

---

## 接线

**这个示例不接任何硬件。** 两个传感器都是 stub：

- **PIR** — 一个 15 秒周期的 `esp_timer` 来回翻转 `s_occupied`，
  站在代码里的位置和真 PIR 的中断完全一样
- **照度** — `on_read()` 返回 480 或者 80 lux，跟着人在状态变

所以烧进去就能在 HA 里看到一个每 15 秒翻一次的人在传感器和一个跟着跳的照度值。
**先确认链路，再接硬件**——"换成真硬件"一节给了 HC-SR501 和 BH1750 的完整写法。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/occupancy_sensor
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

```
espnow2mqtt/occ1/availability online
espnow2mqtt/occ1/state          {"occupancy":"ON","illuminance":480,
                                 "caps":["occupancy","illuminance"],"hop":1}
```

每 15 秒 `occupancy` 在 `ON` / `OFF` 之间翻一次，`illuminance` 跟着在 480 / 80 之间跳。
照度单位 **lux**，是整数（HA 那边显示精度 0 位）。

---

## 两条路各走各的

```
人在（推）：PIR 中断 / 定时器 → en2m_schedule(publish_motion)
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

### `en2m_schedule` 和 `en2m_schedule_from_isr` 的区别

```c
static void simulate_motion(void *arg)      // 跑在 esp_timer 任务上
{
    s_occupied = !s_occupied;
    en2m_schedule(publish_motion, NULL);    // ← 不带 _from_isr
}
```

这里用的是 `en2m_schedule()`，因为 `esp_timer` 的回调跑在一个**普通任务**上，
不是中断上下文。

- 从**任务**里调 → `en2m_schedule()`
- 从**中断**里调 → `en2m_schedule_from_isr()` + `portYIELD_FROM_ISR()`

接真 PIR 的话就要换成后者，写法见
[`contact_sensor`](../contact_sensor/README.md#en2m_schedule_from_isr-那三行)。

**为什么不在 `esp_timer` 回调里直接调 `en2m_report_occupancy()`？**
技术上可以（那不是中断上下文），但 `en2m_schedule` 把所有数据模型操作
收拢到一个任务上，省掉了一整类竞态问题。这个库里所有"改状态"的操作
都应该走 en2m 任务，见 [docs/concurrency.md](../../docs/concurrency.md)。

### 顺序：`en2m_start` 之后才启动事件源

```c
ESP_ERROR_CHECK(en2m_start(&cfg));
ESP_ERROR_CHECK(start_motion_simulation());   // ← 在后面
```

反过来写的话，定时器可能在 `en2m_start()` 建好队列之前就触发，
那次 `en2m_schedule()` 会直接返回错误——第一次动作就丢了。

**通用规则：事件源（中断、定时器、任务）一律在 `en2m_start()` 之后启动。**
但硬件初始化（`gpio_config`、I²C 初始化）要在**之前**，
因为持久化状态的恢复会在 `en2m_start()` 里调你的写回调。

---

## 换成真硬件

### PIR：HC-SR501 / AM312

这类模块是**数字输出**：检测到动作就把一个引脚拉高几秒（SR501 上有个电位器调时长）。

```c
#define PIN_PIR GPIO_NUM_6

static void publish_motion(void *arg)
{
    en2m_report_occupancy(ENDPOINT, gpio_get_level(PIN_PIR) == 1);
}

static void IRAM_ATTR on_pir_edge(void *arg)
{
    BaseType_t woken = pdFALSE;
    en2m_schedule_from_isr(publish_motion, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* app_main 里，en2m_start 之前配 GPIO： */
gpio_config_t io = {
    .pin_bit_mask = 1ULL << PIN_PIR,
    .mode = GPIO_MODE_INPUT,
    .intr_type = GPIO_INTR_ANYEDGE,      // ← 两个边沿都要，才能报 OFF
};
gpio_config(&io);
gpio_install_isr_service(0);
gpio_isr_handler_add(PIN_PIR, on_pir_edge, NULL);
```

注意 `GPIO_INTR_ANYEDGE`：只抓上升沿的话你永远报不出"人走了"。

**HC-SR501 要 5 V 供电**（VCC 接 5 V，输出是 3.3 V 电平，能直接进 C3）。
AM312 是 3.3 V 的，更适合电池设备。

### 毫米波：LD2410 / LD2450

比 PIR 好得多——能检测**静止的人**（PIR 只对移动敏感，人坐着不动就报"没人"）。
接口是 UART，需要一个解析任务：

```c
/* 解析任务里，拿到一帧之后： */
if (presence != s_last_presence) {
    s_last_presence = presence;
    en2m_schedule(publish_motion, NULL);     // 从任务里调，不带 _from_isr
}
```

LD2410 还能给出距离，可以额外挂一个自定义属性上去。

### 照度：BH1750 / TSL2591（I²C）

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_ILLUMINANCE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t lux = 0;
    ESP_RETURN_ON_ERROR(bh1750_read(&lux), TAG, "bh1750 failed");
    *out = en2m_u32(lux);
    return ESP_OK;
}
```

BH1750 的单次高分辨率测量约 120 ms，**这个时长放在 `on_read()` 里偏长了**。
干净的做法是用连续测量模式（上电时配一次），`on_read()` 里只读寄存器（几毫秒）。

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
| 只报 `ON` 从不报 `OFF` | 中断只配了上升沿。要 `GPIO_INTR_ANYEDGE` |
| PIR 一直报有人 | 模块对着热源（暖气、显示器、阳光直射）；或者 5 V 供电不稳 |
| 照度一直是同一个数 | `on_read()` 返回了错误，或者 BH1750 在单次模式下没重新触发测量 |
| 上线后头几秒 HA 里"不可用" | 正常，第一条上报之前 HA 不知道有哪些 cap |
| 人在状态漏一次 | ESP-NOW 丢包。这个示例没有周期兜底，要加的话在 `on_read()` 里也返回 occupancy |

最后那条值得展开：**这个示例的人在状态没有自愈路径**，
跟 [`contact_sensor`](../contact_sensor) 不一样。
接真 PIR 的时候建议在 `on_read()` 里也处理 `EN2M_CLUSTER_OCCUPANCY`
（直接 `gpio_get_level`），这样每个周期上报都会重新对一次。

---

## 延伸阅读

- [docs/examples.md#occupancy_sensor](../../docs/examples.md#occupancy_sensor) — 逐行精讲
- [docs/concurrency.md](../../docs/concurrency.md) — `en2m_schedule` 家族和任务模型
- [docs/reporting.md](../../docs/reporting.md) — 推送上报和周期上报的关系

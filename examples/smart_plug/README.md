# `smart_plug` — 计量插座

**混用两种回调方向的示例。** 继电器靠**写回调**驱动（远程命令推过来），
电量读数靠**读回调**拉取（要上报了组件才来问）。
这两件事的节奏完全不同，所以它们走不同的回调。

| | |
|---|---|
| Cluster | OnOff (`0x0006`) + Electrical Power Measurement (`0x0B04`) |
| 设备类型 | `EN2M_DEVICE_TYPE_SMART_PLUG` |
| 回调 | `attribute_write` + `attribute_read` |
| 驱动 | `drv_gpio_relay`、`drv_gpio_button` |
| 上报周期 | 15 s（`report_interval_ms`） |
| 默认名 / slug | `plug1` |
| HA 实体 | `switch.plug1` + `sensor.plug1_power` + `sensor.plug1_energy` |

---

## 接线

和 [`relay_switch`](../relay_switch) 完全一样：

| GPIO | 接什么 | 说明 |
|---|---|---|
| **5** | 继电器模块 `IN` | 高电平有效 |
| **9** | 按键到 GND | 板载 BOOT 键 |

**计量芯片没有接线**，因为这个示例的计量是个 stub：
`meter_sample()` 用 `esp_random()` 编了一个 20–60 W 的读数。
换成 BL0937 / HLW8012 见下文。

> 插座是要接 220 V 的东西。强电部分请用成品模块，
> 并且读一遍 [docs/wiring.md 的上电默认电平那一节](../../docs/wiring.md#上电默认电平)。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/smart_plug
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

```
espnow2mqtt/plug1/availability online
espnow2mqtt/plug1/state        {"switch":"ON","power":41.3,"energy":1.72,
                                "caps":["switch","power","energy"],"hop":1}
```

- `power` 单位 **W**（固件内部是 mW，序列化时 `/1000`）
- `energy` 单位 **Wh**（固件内部是 mWh，同样 `/1000`）。
  HA 那边 `sensor.plug1_energy` 的 `state_class` 是 `total_increasing`，
  能直接进能量面板

每 15 秒一条新的。开关关掉之后 `power` 会变成 0，`energy` 停住不动。

```bash
mosquitto_pub -t espnow2mqtt/plug1/set -m '{"switch":"OFF"}'
```

---

## 为什么计量要用读回调

传感器有两种写法，选哪种取决于**采样贵不贵**：

| 写法 | 什么时候用 | 这个示例 |
|---|---|---|
| `attribute_read` 回调（拉） | 采样便宜，随时能读 | ✅ 功率、电量 |
| `en2m_attribute_set()` 主动推 | 采样贵，或者数据是中断来的 | ❌ |

计量 IC 的寄存器随时能读，所以没必要自己开定时器缓存。
组件在**准备上报的那一刻**才调 `on_read()`，于是采样频率自动等于上报频率——
你改 `report_interval_ms`，采样跟着变，不用改第二个地方。

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_ELECTRICAL_POWER) {
        return ESP_ERR_NOT_SUPPORTED;    // ← 其它 cluster 用数据模型里的缓存值
    }
    if (path->attribute_id == EN2M_ATTR_ACTIVE_POWER_MW) { ... }
    if (path->attribute_id == EN2M_ATTR_ENERGY_MWH)      { ... }
    return ESP_ERR_NOT_SUPPORTED;
}
```

**`ESP_ERR_NOT_SUPPORTED` 不是错误。** 它告诉组件"这个属性我不管，
你用数据模型里存的值"。OnOff 属性就是这么走的：它的真值在数据模型里，
不需要回读 GPIO。

---

## 累计电量怎么做

`meter_sample()` 用**上一次采样的功率乘以两次采样的间隔**来累加：

```c
if (s_meter.last_sample_us != 0) {
    int64_t elapsed_us = now - s_meter.last_sample_us;
    s_meter.energy_mwh += (int64_t)s_meter.power_mw * elapsed_us / (3600LL * 1000000LL);
}
s_meter.last_sample_us = now;
```

三个细节值得抄：

1. **先积分再取新值**。反过来写的话，你用新功率去乘刚过去的那段时间，
   开关机瞬间误差会很大。
2. **`int64_t`**。`mW · µs` 这个量级在 32 位里几分钟就溢出了。
3. **单位在固件里用整数**（mW、mWh），只在序列化的时候转成 W / kWh。
   浮点在 C3 上没有硬件支持，而且累加浮点会积累误差。

### 这个示例的 `energy` 重启会归零，但原因不是"没持久化"

`EN2M_ATTR_ENERGY_MWH` 这个属性**是持久化的**，重启之后 `en2m_start()`
会把上次的值从 NVS 读回数据模型。

**但这个示例还是从 0 开始数。** 因为累计值实际存在应用自己的
`s_meter.energy_mwh` 里，那是一个静态变量，开机就是 0；
而 `on_read()` 每次都用它覆盖数据模型里的值。**读回调赢了持久化。**

这是一个很容易踩的坑：**只要某个属性有读回调，持久化就只是个摆设**——
回调返回什么就是什么。要让累计值真的接上，得在 `en2m_start()`
之后把持久化的值读回来当种子：

```c
ESP_ERROR_CHECK(en2m_start(&cfg));

en2m_value_t stored;
if (en2m_attribute_get(ENDPOINT, EN2M_CLUSTER_ELECTRICAL_POWER,
                       EN2M_ATTR_ENERGY_MWH, &stored) == ESP_OK) {
    s_meter.energy_mwh = en2m_value_as_int(&stored);
}
```

（必须在 `en2m_start()` **之后**，那之前 NVS 还没回放。）

HA 的能量面板用的是 `state_class: total_increasing`，归零它能容忍
（识别成"表被换了"），但你会丢掉历史曲线。
NVS 的刷盘时机和擦写寿命见
[docs/persistence.md](../../docs/persistence.md)——
别指望它能扛住每 15 秒写一次。

---

## 换成真的计量 IC

BL0937 / HLW8012 这类芯片把功率输出成**脉冲频率**，所以：

1. 用 `gpio_install_isr_service` + 一个脉冲计数 ISR（ISR 里只 `count++`）
2. `on_read()` 里把计数除以时间窗口，再乘芯片的标定系数

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    if (path->attribute_id == EN2M_ATTR_ACTIVE_POWER_MW) {
        uint32_t pulses = atomic_exchange(&s_cf_pulses, 0);
        *out = en2m_i32(pulses * PULSE_TO_MW / window_s);
        return ESP_OK;
    }
    ...
}
```

标定系数必须**实测**：接一个已知功率的负载（比如白炽灯泡），
数脉冲，反推系数。datasheet 上的典型值和实际板子差得不少。

I²C 的计量芯片（INA219、PZEM-004T）更简单，`on_read()` 里直接读寄存器就行，
但注意 `on_read()` 跑在 en2m 任务上，**别在里面做几百毫秒的阻塞 I/O**——
会拖慢整个节点的收发。超过 ~50 ms 的采样就改成后台采、`on_read()` 只取缓存。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| HA 里只有开关，没有两个 sensor | `caps` 里没有 `power`/`energy`。说明 `on_read()` 一直返回错误，看串口日志 |
| `power` 一直是 0 | `drv_gpio_relay_get()` 读出来是关的。示例的 stub 在关机时故意报 0 |
| 能量曲线有尖刺 | 上报丢了一帧，`energy` 跳变。这是正常的，HA 的 `total_increasing` 会处理 |
| 上报变慢或者收发卡顿 | `on_read()` 里做了阻塞 I/O，见上一节 |

---

## 延伸阅读

- [docs/examples.md#smart_plug](../../docs/examples.md#smart_plug) — 逐行精讲
- [docs/callbacks.md](../../docs/callbacks.md) — `read` 回调的线程和返回值约定
- [docs/reporting.md](../../docs/reporting.md) — `report_interval_ms` 和上报限流

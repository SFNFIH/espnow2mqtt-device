# `th_sensor` — 温湿度传感器

**最小的传感器示例：只有一个读回调，没有别的。**
组件在准备上报的那一刻来问温度和湿度，你现场读一下 DHT 就完事。
不需要采样任务，不需要缓存，不需要定时器。

| | |
|---|---|
| Cluster | Temperature Measurement (`0x0402`) + Relative Humidity (`0x0405`) |
| 设备类型 | `TEMPERATURE_SENSOR` + `HUMIDITY_SENSOR`（**两个叠在同一个 endpoint 上**） |
| 回调 | 只有 `attribute_read` |
| 驱动 | `drv_dht` |
| 上报周期 | 60 s，最快 5 s（`min_report_interval_ms`） |
| 默认名 / slug | `th_sensor1` |
| HA 实体 | `sensor.th_sensor1_temperature` + `sensor.th_sensor1_humidity` |

---

## 接线

| GPIO | 接什么 | 说明 |
|---|---|---|
| **4** | DHT22 的 `DATA` | **必须加 4.7–10 kΩ 上拉到 3V3** |

```
        3V3 ──┬── DHT22 VCC
              │
             [10k]
              │
   GPIO4 ─────┴── DHT22 DATA

        GND ────── DHT22 GND
```

**没有上拉电阻就读不出数据**（一直超时）。有些模块板上已经带了，
看板子上有没有一个贴片电阻；裸元件的 DHT22 一定要自己加。

`DHT_TYPE` 是 `22`。要用 DHT11 就改成 `11`——
两者的时序和数据格式不同，驱动里按这个值分支。

DHT22 精度 ±0.5 °C / ±2 %RH，够做房间温控；要更准就换 SHT31 / BME280（I²C），
改法见下文。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/th_sensor
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

```
espnow2mqtt/th_sensor1/availability online
espnow2mqtt/th_sensor1/state    {"temperature":23.4,"humidity":55.1,
                                 "caps":["temperature","humidity"],"hop":1}
```

每 60 秒一条。`temperature` 单位 °C，`humidity` 单位 %，都是**一位小数**——
固件内部用的是厘度 / 厘百分比（`int16_t` 2340 表示 23.40 °C），
序列化的时候 `/100`。

HA 那边两个 sensor 的 `suggested_display_precision` 都是 1 位，
所以界面上显示 `23.4 °C`，不会出现 `23.400000000000002`。

---

## 为什么只写一个读回调就够了

DHT 随时能读（拉低一段电平，它就把数据吐出来），采样成本低，
所以完全不需要自己维护一份缓存。组件的流程是：

```
上报定时器到 → 逐个属性调 on_read() → 拿到值 → 组帧 → 发出去
```

采样频率因此**自动等于上报频率**。你改 `report_interval_ms`，采样跟着变，
不用改第二处代码。

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    switch (path->cluster_id) {
    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT: {
        int16_t centi = 0;
        ESP_RETURN_ON_ERROR(drv_dht_get_temperature(&centi, ctx), TAG, "temperature read failed");
        *out = en2m_i16(centi);
        return ESP_OK;
    }
    case EN2M_CLUSTER_RELATIVE_HUMIDITY: { ... }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
```

`ESP_RETURN_ON_ERROR` 在读失败时**打一条日志然后返回错误码**。
返回错误的后果是这个属性**这一轮不进上报**——
上报里少一个字段，比发一个 `-999` 或者上一轮的旧值要好，
HA 会保留上一个已知值而不是把曲线拉到坑里。

### `min_report_interval_ms` 是保护 DHT 的

```c
.report_interval_ms = 60000,
.min_report_interval_ms = 5000,
```

**DHT22 的采样间隔不能短于 2 秒**，读太快它会返回上一次的缓存值甚至直接超时。
`min_report_interval_ms` 给上报做了 5 秒的下限，
所以就算有一串变化事件挤在一起，`on_read()` 也不会被 5 秒内调两次。

这个参数对**任何有最小采样间隔的传感器**都要设：DHT、部分 CO₂ 模块、
需要加热周期的 VOC 传感器。见 [docs/reporting.md](../../docs/reporting.md)。

---

## 两个设备类型叠在一个 endpoint 上

```c
ep = en2m_endpoint_create(ENDPOINT);
en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR);
en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_HUMIDITY_SENSOR);
```

注意这里用的是 `en2m_endpoint_create()` + 两次 `add_device_type()`，
而不是 `en2m_endpoint_create_device()`（那个只能带一个类型）。

结果是**一个 HA 设备下面两个实体**，而不是两个设备。
这才是你想要的：物理上就是一颗芯片，不该在 HA 里显示成两台机器。

加第三个传感器（比如气压）就再来一行：

```c
en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_PRESSURE_SENSOR);
```

然后在 `on_read()` 里加一个 `case EN2M_CLUSTER_PRESSURE_MEASUREMENT`。
每个 endpoint 默认最多 6 个 cluster，见 [docs/kconfig.md](../../docs/kconfig.md)。

---

## 换成别的传感器

### SHT31 / SHT41（I²C，推荐）

比 DHT22 准，而且是标准 I²C，没有时序坑：

```c
case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT: {
    float t, rh;
    ESP_RETURN_ON_ERROR(sht3x_measure(&t, &rh), TAG, "sht3x failed");
    *out = en2m_i16((int16_t)(t * 100));
    return ESP_OK;
}
```

I²C 的单次测量大约 15 ms，放在 `on_read()` 里没问题。

### BME280 / BME680

多一个气压（和 BME680 的 VOC）。气压走
`EN2M_CLUSTER_PRESSURE_MEASUREMENT`，单位是 **0.1 hPa**（序列化时 `/10`）。

### 要注意的一条通用规则

`on_read()` **跑在 en2m 任务上**。如果你的采样要几百毫秒
（DS18B20 的 12 位转换 750 ms、需要加热的气体传感器），
**不要在 `on_read()` 里等**。改成：

1. 起一个 `esp_timer` 周期采样，存到一个静态变量里
2. `on_read()` 只返回那个静态变量

阻塞 en2m 任务会拖慢整个节点的收发，严重的话会丢上行帧。
见 [docs/concurrency.md](../../docs/concurrency.md)。

---

## 电池供电

这个示例是**常电思路**：每 60 秒醒一次、发一次。
电池设备应该改成深睡：

```c
.report_mode = EN2M_REPORT_ON_CHANGE_ONLY,
```

再配合 `esp_deep_sleep_start()`。注意深睡会丢掉 RAM 里的数据模型，
每次醒来要重新 `en2m_start()`，入网大约要几十到几百毫秒。
一天 24 条上报的温湿度计用两节 AA 能跑很久；一分钟一条就别想了。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 上报里完全没有 `temperature` | `drv_dht_get_temperature()` 一直失败。90% 是缺上拉电阻 |
| 读数偶尔跳到很离谱的值 | DHT22 的校验没过但驱动放过了，或者线太长（>20 cm 就该加屏蔽） |
| 温度一直偏高 1–3 °C | 传感器离 C3 太近，芯片自热。**传感器必须离开主板**，或者主板挖空 |
| 湿度一直 99% | 传感器受潮了。DHT22 在高湿环境里会饱和，通风几小时能恢复 |
| 每分钟只有一条但 HA 里显示"不可用" | 见 [docs/troubleshooting.md](../../docs/troubleshooting.md)，通常是 availability 超时设得比上报周期还短 |

---

## 延伸阅读

- [docs/examples.md#th_sensor](../../docs/examples.md#th_sensor) — 逐行精讲
- [docs/reporting.md](../../docs/reporting.md) — 上报周期、限流、降级
- [docs/concurrency.md](../../docs/concurrency.md) — 哪个回调跑在哪个任务上

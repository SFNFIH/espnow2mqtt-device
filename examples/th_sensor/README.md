# `th_sensor` — 温湿度传感器

**最小的传感器示例：只有一个读回调，没有别的。**
组件在准备上报的那一刻来问温度和湿度，你现场读一下传感器就完事。
不需要采样任务，不需要定时器。

| | |
|---|---|
| Cluster | Temperature Measurement (`0x0402`) + Relative Humidity (`0x0405`) |
| 设备类型 | `TEMPERATURE_SENSOR` + `HUMIDITY_SENSOR`（**两个叠在同一个 endpoint 上**） |
| 回调 | 只有 `attribute_read` |
| 外设组件 | [`espressif/aht20`](https://components.espressif.com/components/espressif/aht20) `^2.0.0` |
| 上报周期 | 60 s，最快 5 s（`min_report_interval_ms`） |
| 默认名 / slug | `th_sensor1` |
| HA 实体 | `sensor.th_sensor1_temperature` + `sensor.th_sensor1_humidity` |

---

## 接线

AHT20 是 I²C 器件，四根线：

| GPIO | 接什么 | 说明 |
|---|---|---|
| **4** | AHT20 `SDA` | 代码里打开了内部上拉 |
| **5** | AHT20 `SCL` | 同上 |
| 3V3 | AHT20 `VCC` | **不能接 5 V** |
| GND | AHT20 `GND` | |

引脚是 `main/main.c` 顶部的 `#define PIN_SDA` / `#define PIN_SCL`，改一行就换。
换引脚前先看 [docs/wiring.md](../../docs/wiring.md#esp32-c3-引脚选择须知)。

I²C 地址默认 `0x38`（`AHT20_ADDRRES_0`）。模块上的 `CE`/`AD` 脚拉高的话是 `0x39`，
改 `sensor_init()` 里的 `.i2c_addr`。

**内部上拉够用，但只在短线上够用。** C3 的内部上拉是 45 kΩ 左右，
线超过 20 cm 或者总线上挂了多个器件，就该外加 4.7 kΩ 到 3V3。
读不出来先怀疑这个。

### 为什么是 AHT20 而不是 DHT22

因为 [ESP 组件注册表](https://components.espressif.com)里**没有 DHT 组件**，
而这个仓库的原则是外设驱动一律来自注册表，不自己手写。

这个替换其实是纯赚的：AHT20 和 DHT22 量程一样（−40…85 °C / 0…100 %RH），
精度更好（±0.3 °C / ±2 %RH），走标准 I²C 所以没有单总线的时序坑，
也不需要那颗必须外挂的上拉电阻，模块价格还更便宜。

要用 SHT3x 的话注册表里有
[`espressif/sht3x`](https://components.espressif.com/components/espressif/sht3x)，
换法见下文。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/th_sensor
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

第一次 `build` 会联网下载 `espressif/aht20` 和它的公开依赖
`espressif/i2c_bus` 到本工程的 `managed_components/`。

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

采样很便宜（一次 I²C 事务，约 80 ms），所以完全不需要自己起采样任务。
组件的流程是：

```
上报定时器到 → 逐个属性调 on_read() → 拿到值 → 组帧 → 发出去
```

采样频率因此**自动等于上报频率**。你改 `report_interval_ms`，采样跟着变，
不用改第二处代码。

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    switch (path->cluster_id) {
    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT:
        ESP_RETURN_ON_ERROR(sample(), TAG, "temperature read failed");
        *out = en2m_i16((int16_t)(s_sample.celsius * 100.0f));
        return ESP_OK;
    case EN2M_CLUSTER_RELATIVE_HUMIDITY:
        ESP_RETURN_ON_ERROR(sample(), TAG, "humidity read failed");
        *out = en2m_u16((uint16_t)(s_sample.humidity * 100.0f));
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
```

`ESP_RETURN_ON_ERROR` 在读失败时**打一条日志然后返回错误码**。
返回错误的后果是这个属性**这一轮不进上报**——
上报里少一个字段，比发一个 `-999` 或者上一轮的旧值要好，
HA 会保留上一个已知值而不是把曲线拉到坑里。

### 一次测量供两个属性用

AHT20 一次转换**同时**给出温度和湿度，但 `on_read()` 是**每个属性调一次**的。
如果两次调用各测一遍，一轮上报就要等 160 ms，而且两个数还来自不同时刻。

所以 `sample()` 带一个时间戳缓存：

```c
#define SAMPLE_CACHE_MS 2000

static esp_err_t sample(void)
{
    int64_t now = esp_timer_get_time();

    if (s_sample.taken_us != 0 && now - s_sample.taken_us < SAMPLE_CACHE_MS * 1000LL) {
        return ESP_OK;                      /* 上一次的还新鲜，直接复用 */
    }
    ESP_RETURN_ON_ERROR(aht20_read_temperature_humidity(...), TAG, "aht20 read failed");
    ...
}
```

同一轮上报里的第二次调用落在 2 秒窗口内，**直接吃缓存**。
好处是：同一条上报里的温度和湿度来自同一次测量，时间上是一致的；
而且**不依赖属性被遍历的顺序**——谁先被问到谁去测，另一个复用，
两种顺序结果都一样。

窗口设成 2 秒是因为上报最快 5 秒一次（见下），
所以缓存**绝不会跨轮复用**，每一轮都是新鲜数据。
你要是把 `min_report_interval_ms` 调到 2 秒以下，记得把这个窗口一起调小。

### `min_report_interval_ms` 是保护传感器的

```c
.report_interval_ms = 60000,
.min_report_interval_ms = 5000,
```

上报有 5 秒下限，所以就算一串变化事件挤在一起，
`on_read()` 也不会被 5 秒内调两轮。

AHT20 自己不怕读得快，但**自热会影响读数**：
连续不停地测，芯片温度会比环境高个零点几度。5 秒的下限顺手把这件事也解决了。

这个参数对**任何有最小采样间隔的传感器**都要设：部分 CO₂ 模块、
需要加热周期的 VOC 传感器、12 位的 DS18B20。
见 [docs/reporting.md](../../docs/reporting.md)。

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
每个 endpoint 默认最多 8 个 cluster、每个 cluster 最多 6 个属性，
上限见 [docs/kconfig.md](../../docs/kconfig.md)。

---

## 换成别的传感器

### SHT3x

注册表里有 [`espressif/sht3x`](https://components.espressif.com/components/espressif/sht3x)，
和 `aht20` 一样挂在 `espressif/i2c_bus` 上，所以 `sensor_init()` 里
建总线那段一行都不用改，只换器件那两行：

```yaml
# main/idf_component.yml
dependencies:
  espressif/sht3x: "^0.3.0"
```

```c
static sht3x_handle_t s_sht3x;

s_sht3x = sht3x_create(bus, SHT3x_ADDR_PIN_SELECT_VSS);   /* 0x44 */
...
sht3x_get_single_shot(s_sht3x, &celsius, &humidity);
```

`sht3x_get_single_shot()` 一次也同时给温度和湿度，
所以上面那套缓存原样能用。精度略好于 AHT20（±0.2 °C），价格贵一点。

### 加一个气压

注册表里有
[`espressif/bme280`](https://components.espressif.com/components/espressif/bme280)，
同样挂在 `espressif/i2c_bus` 上。气压走 `EN2M_CLUSTER_PRESSURE_MEASUREMENT`，
单位是 **0.1 hPa**（序列化时 `/10`）。
可以挂在同一条 I²C 总线上：`i2c_bus_create()` 是**单例**的，
同一个 port 调第二次直接返回已有的句柄，不会把前一个配置踩掉。

### 要注意的一条通用规则

`on_read()` **跑在 en2m 任务上**。如果你的采样要几百毫秒
（DS18B20 的 12 位转换 750 ms、需要加热的气体传感器），
**不要在 `on_read()` 里等**。改成：

1. 起一个 `esp_timer` 周期采样，存到一个静态变量里
2. `on_read()` 只返回那个静态变量

AHT20 的 80 ms 是在"还能忍"的范围内，所以这个示例图省事直接读了。
再长就必须挪出去。阻塞 en2m 任务会拖慢整个节点的收发，严重的话会丢上行帧。
见 [docs/concurrency.md](../../docs/concurrency.md)。

（照度传感器有个更漂亮的办法：让它跑连续测量模式，
`on_read()` 只取一次寄存器。见 [`occupancy_sensor`](../occupancy_sensor)。）

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
| 启动就 `ESP_ERROR_CHECK` 挂在 `sensor_init()` | I²C 上找不到 0x38。先查接线和上拉，再确认地址不是 0x39 |
| 上报里完全没有 `temperature` | `aht20_read_temperature_humidity()` 一直失败。线太长或者缺外部上拉 |
| 温湿度都是 0 | 传感器刚上电没校准。AHT20 首次上电需要约 100 ms 初始化，正常几轮之后就好 |
| 温度一直偏高 1–3 °C | 传感器离 C3 太近，芯片自热。**传感器必须离开主板**，或者主板挖空 |
| 湿度一直 99% | 传感器受潮饱和了。通风几小时能恢复 |
| 每分钟只有一条但 HA 里显示"不可用" | 见 [docs/troubleshooting.md](../../docs/troubleshooting.md)，通常是 availability 超时设得比上报周期还短 |

---

## 延伸阅读

- [docs/examples.md#th_sensor](../../docs/examples.md#th_sensor) — 逐行精讲
- [docs/reporting.md](../../docs/reporting.md) — 上报周期、限流、降级
- [docs/concurrency.md](../../docs/concurrency.md) — 哪个回调跑在哪个任务上

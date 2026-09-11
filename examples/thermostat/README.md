# `thermostat` — 温控器

**三个回调配合跑一个设备端闭环的示例，也是最复杂的一个。**

温控器和前面所有示例都不一样：它不是"收到命令就执行"，
而是**自己持续做决定**。房间冷了就该开热，到温了就该停——
这个判断必须在设备上做，不能依赖 HA。**HA 重启、网络断了，房间也不能冻着。**

| | |
|---|---|
| Cluster | Thermostat (`0x0201`) |
| 设备类型 | `EN2M_DEVICE_TYPE_THERMOSTAT` |
| 回调 | `attribute_write` + `attribute_read` + `attribute_changed` |
| 驱动 | 无（温度是个会自己漂移的 stub） |
| 默认名 / slug | `thermo1` |
| HA 实体 | `climate.thermo1` |

---

## 接线

**这个示例不接任何硬件。** 两个 stub：

- **室温** — `on_read()` 里的假传感器：继电器开着就每次读数 +0.05 °C，
  关着就 −0.02 °C。所以你能看到它真的在追设定点
- **热需求继电器** — 一行 `ESP_LOGI`

这个漂移 stub 是有意设计的：**烧进去不接任何东西，就能看到闭环在工作**。
串口上会看到继电器在设定点附近开开关关。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/thermostat
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

```
espnow2mqtt/thermo1/availability online
espnow2mqtt/thermo1/state  {"hvac_mode":"off","current_temperature":22.0,
                            "target_temperature":21.0,"caps":["climate"],"hop":1}
```

第一次上电 `hvac_mode` 是 `off`，因为 `EN2M_ATTR_SYSTEM_MODE` 的默认值是
`EN2M_THERMOSTAT_OFF`。`target_temperature` 是 21.0，
来自制热设定点的默认值 2100（厘度）。

串口上能看到闭环：

```
I (18204) ex_climate: demand relay on        ← 室温掉到 20.8 以下
I (94301) ex_climate: demand relay off       ← 升到 21.2 以上
```

手动控制：

```bash
mosquitto_pub -t espnow2mqtt/thermo1/set -m '{"hvac_mode":"heat"}'
mosquitto_pub -t espnow2mqtt/thermo1/set -m '{"target_temperature":23.5}'
mosquitto_pub -t espnow2mqtt/thermo1/set -m '{"hvac_mode":"off"}'
```

五种模式：`off` / `auto` / `cool` / `heat` / `fan_only`
（HA 的 `climate` 实体里就是那个模式下拉框）。

---

## 四个属性的分工

| 属性 | 谁写 | 说明 |
|---|---|---|
| `EN2M_ATTR_SYSTEM_MODE` | HA 写 | `off` / `auto` / `cool` / `heat` / `fan_only` |
| `EN2M_ATTR_OCCUPIED_HEATING_SETPOINT` | HA 写 | 制热设定点，厘度（2100 = 21.00 °C） |
| `EN2M_ATTR_OCCUPIED_COOLING_SETPOINT` | HA 写 | 制冷设定点 |
| `EN2M_ATTR_LOCAL_TEMPERATURE` | **设备读**，也可以外部写 | 实测室温 |

`target_temperature` 这个 MQTT 字段**不是一个属性**，它是两个设定点里的一个：

```c
/* 序列化时按当前模式选 */
(v == EN2M_THERMOSTAT_COOL) ? EN2M_ATTR_OCCUPIED_COOLING_SETPOINT
                            : EN2M_ATTR_OCCUPIED_HEATING_SETPOINT
```

HA 那边只有**一个**目标温度滑块，所以上下行都要做这个映射：
上报时报"当前模式正在追的那个设定点"，
收到 `{"target_temperature":23.5}` 时也按当前模式写进对应的那一个。

这样在 `heat` 模式下拖滑块不会把制冷设定点搞乱，反之也一样。
切模式的时候滑块会跳到另一个设定点上——这是对的，不是 bug。

---

## 三个回调各干什么

```
on_write()    ← HA 改模式或者设定点  → 存进本地状态 → 立刻重算一次
on_read()     ← 要上报了，给我室温  → 读传感器（这个示例里是漂移 stub）
on_changed()  ← 室温这个属性变了    → 重算一次
```

关键在于**闭环不是一个循环，是一串事件**：

| 触发 | 走哪个回调 | 结果 |
|---|---|---|
| 用户改设定点 | `on_write` | 立刻重算，可能马上开 / 停 |
| 上报周期到了 | `on_read` → `on_changed` | 拿到新室温，重算 |
| 外部传感器写入室温 | `on_changed` | 重算 |

**所以没有 `while (1)`，也没有 `vTaskDelay`。**
`hvac_control()` 只在有新信息的时候被调，其它时候 CPU 在睡。

### 为什么 `LOCAL_TEMPERATURE` 既有读回调又在 `changed` 里处理

看着像重复，其实覆盖的是两个不同来源：

```c
static esp_err_t on_read(...)     // 来源 A：本机传感器
{
    if (path->attribute_id != EN2M_ATTR_LOCAL_TEMPERATURE) return ESP_ERR_NOT_SUPPORTED;
    ...
    *out_value = en2m_i16(s_hvac.local_centi);
}

static void on_attribute_changed(...)  // 来源 B：任何人写了这个属性
{
    if (path->attribute_id == EN2M_ATTR_LOCAL_TEMPERATURE) {
        s_hvac.local_centi = value->v.i16;
        hvac_control();
    }
}
```

`on_changed` 那条路让**远程温度源**能用：
HA 里有一个装在床头的温湿度计，你想让温控器按它的读数工作，
于是 HA 自动化把那个值写到 `thermo1` 的 `LOCAL_TEMPERATURE` 上。

这是很常用的配置（"按房间平均温度控制"），
而且**不需要改一行固件**——两条路已经都在了。

### 回差（hysteresis）

```c
case EN2M_THERMOSTAT_HEAT:
    want = s_hvac.local_centi < s_hvac.heating_centi - 20;   // 低于 设定点-0.2°C 开
    if (s_hvac.local_centi > s_hvac.heating_centi + 20) {    // 高于 设定点+0.2°C 关
        want = false;
    }
    break;
```

那个 `20`（0.2 °C）是**回差，绝对不能省**。

没有回差的话，室温在设定点附近抖动 0.01 °C 就会让继电器
开-关-开-关，一分钟几十次。后果是：

- 继电器触点几天就烧穿
- 压缩机（空调、热泵）会**损坏**——它们有最短运行时间要求
- 每次切换都触发一次上报，射频被刷爆

**0.2 °C 对水暖地暖偏小，对空调偏小得多。** 实际值：

| 系统 | 建议回差 | 附加保护 |
|---|---|---|
| 电热 / 电暖气 | 0.2–0.5 °C | 无 |
| 燃气壁挂炉 | 0.5 °C | 最短停机 3 分钟 |
| 水地暖 | 0.5–1.0 °C | 惯性很大，别追太紧 |
| 空调 / 热泵 | 0.5 °C | **最短运行 5 分钟 + 最短停机 5 分钟，必须有** |

压缩机类的最短运行时间要自己加一个时间戳判断：

```c
static int64_t s_last_switch_us;

if (want != s_hvac.relay_on) {
    if (esp_timer_get_time() - s_last_switch_us < MIN_CYCLE_US) {
        return;                      // 还没到最短周期，这次不动
    }
    s_last_switch_us = esp_timer_get_time();
    s_hvac.relay_on = want;
    ...
}
```

### `changed` 回调的重入陷阱

`on_changed()` 里**不要**去写同一个属性：

```c
static void on_attribute_changed(...)
{
    /* 千万不要这么写 */
    en2m_attribute_set(endpoint, cluster, path->attribute_id, something);   // ← 无限递归
}
```

`changed` 是"值已经变了"的通知。在里面再改一次会再触发一次 `changed`，
如果新值和旧值不同就一直递归下去。

这个示例里 `on_changed()` 只改了**自己的静态变量** `s_hvac.local_centi`，
没碰数据模型，所以是安全的。

---

## 换成真硬件

### 室温传感器

`on_read()` 里的 stub 换成真读数：

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_THERMOSTAT ||
        path->attribute_id != EN2M_ATTR_LOCAL_TEMPERATURE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    float t;
    ESP_RETURN_ON_ERROR(sht3x_read_temperature(&t), TAG, "sensor failed");
    *out = en2m_i16((int16_t)(t * 100));
    return ESP_OK;
}
```

**传感器的位置比精度重要得多。** 放在 C3 旁边会被芯片自热抬高 1–3 °C，
放在墙上靠近暖气会一直觉得房间很暖。理想位置是离地 1.2–1.5 m、
避开直射阳光和风口。

### 热需求输出

大多数壁挂炉 / 地暖分集水器要的是一个**干接点**（继电器）：

```c
if (want != s_hvac.relay_on) {
    s_hvac.relay_on = want;
    gpio_set_level(PIN_DEMAND, want);
}
```

**炉子那边的接线一定要用无源干接点**，不要直接给电压——
不同品牌的炉子接口电压不一样（24 V AC、230 V、或者 OpenTherm 数字总线），
接错会烧控制板。

### OpenTherm

OpenTherm 能让你控制**出水温度**而不是只能开 / 关，效率高得多。
但它是一个需要精确时序的曼彻斯特编码总线，
要用现成的 OpenTherm 适配器板 + 一个协议库。
这已经超出这个示例的范围了，但接口不用变——
`hvac_control()` 里把"开 / 关"换成"算一个出水温度"就行。

### 三线 / 两线阀门执行器

温控阀（TRV 的电动头、区域阀）通常是：

- **两线常闭**：给电开，断电关。一个继电器就够
- **三线（SPDT）**：开、关、公共。要两个继电器，**必须互锁**，
  参考 [`window_cover`](../window_cover/README.md#交流管状电机卷帘遮阳篷) 里的写法

注意阀门执行器**开到位要 2–5 分钟**（蜡马达式的更慢）。
所以控制周期要比这个长，否则你在不停地半开半关。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 继电器疯狂开关 | 没有回差，或者回差太小。见上文 |
| 压缩机报故障 / 保护 | 缺最短运行 / 停机时间 |
| 切模式后目标温度跳了 | **正常**，两个设定点是分开存的 |
| 刚上电 HA 里显示 `off`，但串口上继电器已经在动 | 见下面一段 |
| 室温一直偏高 | 传感器贴着芯片，自热。把传感器引出去 |
| 重启后设定点回到 21 / 24 | NVS 分区问题，见 [docs/persistence.md](../../docs/persistence.md) |
| HA 里模式下拉只有五项 | **故意的**。集成只暴露固件能解析的那五个，多给会让 HA 发出一个固件默默当成 `off` 的模式 |
| `on_changed` 里死循环 / 栈溢出 | 在 `changed` 里又写了同一个属性 |

### 最后那条值得展开：本地状态和数据模型要对齐

示例里的 `s_hvac` 是**应用自己的静态变量**，它的初值写死成
`.mode = EN2M_THERMOSTAT_HEAT, .heating_centi = 2100, .cooling_centi = 2400`。
而数据模型那边 `SYSTEM_MODE` 的默认值是 `EN2M_THERMOSTAT_OFF`。

**两边不一致**：HA 里看到的是 `off`，但 `hvac_control()` 用的是
`s_hvac.mode == HEAT`，所以继电器真的会动。只要你在 HA 里设一次模式，
`on_write()` 就会把两边对齐，之后一切正常。

这个初值在示例里是为了"烧进去就能看到闭环在跑"。
**真产品应该在 `en2m_start()` 之后把恢复出来的值读回本地**：

```c
ESP_ERROR_CHECK(en2m_start(&cfg));

en2m_value_t v;
if (en2m_attribute_get(ENDPOINT, EN2M_CLUSTER_THERMOSTAT,
                       EN2M_ATTR_SYSTEM_MODE, &v) == ESP_OK) {
    s_hvac.mode = (en2m_thermostat_mode_t)en2m_value_as_int(&v);
}
/* 两个设定点同理 */
hvac_control();
```

模式和两个设定点都是**持久化属性**，所以这么读回来拿到的是断电前用户设的值，
不是默认值。

---

## 延伸阅读

- [docs/examples.md#thermostat](../../docs/examples.md#thermostat) — 逐行精讲
- [docs/callbacks.md](../../docs/callbacks.md) — 三个回调的触发时机和顺序
- [docs/state-flow.md](../../docs/state-flow.md) — 一次设定点修改的完整流转
- [docs/persistence.md](../../docs/persistence.md) — 设定点为什么要持久化

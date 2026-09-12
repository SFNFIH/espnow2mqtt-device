# 示例逐个详解

示例在仓库根目录 **`examples/`**，一个目录一个独立 ESP-IDF 工程。
外设驱动一律来自 **[ESP 组件注册表](https://components.espressif.com)**，
每个工程在自己的 `main/idf_component.yml` 里声明依赖，
`idf.py build` 自动下载到该工程的 `managed_components/`。
对应关系见 [examples/README.md 的外设组件表](../examples/README.md#外设组件)。

> **这篇讲"为什么这么写"。** 想知道"怎么跑起来"——接线、烧写、
> 在 HA 里出什么实体、怎么换成真硬件——看每个示例目录里自己的 README，
> 入口是 [examples/README.md](../examples/README.md)。

十一个示例不是十一个"功能演示"，而是**十一种把硬件接到这个库上的姿势**。
选示例的时候不要只看设备类型像不像，要看**回调组合**像不像——
那个才是你要抄的东西。

## 一览

| 示例 | Cluster | 回调组合 | 演示的核心 |
|---|---|---|---|
| [`relay_switch`](#relay_switch) | OnOff | `write` + `changed` | 远程和本地走同一条路径；按键回调 → `en2m_schedule` |
| [`dimmable_light`](#dimmable_light) | OnOff + Level + ColorControl | `write` + `identify` | 一个回调按 `cluster_id` 分发多个 cluster |
| [`smart_plug`](#smart_plug) | OnOff + ElectricalPower | `write` + `read` | 两种方向混用：执行器靠写，计量靠读 |
| [`th_sensor`](#th_sensor) | Temperature + Humidity | 只有 `read` | 纯拉取型；采样周期用 `min_report_interval_ms` 保护 |
| [`contact_sensor`](#contact_sensor) | BooleanState | `read`（兜底）+ 边沿推送 | 事件驱动 + 周期性自愈 |
| [`scene_switch`](#scene_switch) | Switch | 一个回调都没有 | 纯上行、无状态；`EN2M_REPORT_ON_CHANGE_ONLY` |
| [`occupancy_sensor`](#occupancy_sensor) | Occupancy + Illuminance | `read` + `en2m_schedule` 推送 | 一个 endpoint 上推、拉两种传感器并存 |
| [`fan_controller`](#fan_controller) | FanControl | 按 cluster 注册的 `write` | `en2m_cluster_set_write_cb` + 私有 `ctx` |
| [`window_cover`](#window_cover) | WindowCovering | 只有 `command` | 慢执行器：命令只管启停，位置自己上报 |
| [`door_lock`](#door_lock) | DoorLock | 只有 `write` | 持久化：开机把上次状态写回执行器 |
| [`thermostat`](#thermostat) | Thermostat | `write` + `read` + `changed` | 三个回调配合跑设备端闭环 |
| [`firmware/router`](#firmwarerouter) | 无 | 只有事件 | 纯传输层：`en2m_mesh_init` 不带数据模型 |

**十一个示例都没有 `while (1)`，也没有一个自己建的任务。**
用到按键的四个示例间接借用了 `espressif/button` 的那个共享 esp_timer，
但那是组件的事，应用代码里一行任务代码都没有。

## 怎么编译

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/relay_switch
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

每个工程的 `CMakeLists.txt` 里已经写好 `EXTRA_COMPONENT_DIRS` 指向
`../../components`，不需要额外配置。`sdkconfig.defaults` 里已经设好目标芯片、
4 MB flash、USB-Serial-JTAG 控制台和 `FREERTOS_HZ=1000`。

**第一次 `build` 需要联网**：`idf_component.yml` 里声明的注册表组件会被下载到
本工程的 `managed_components/`。那个目录是构建产物，不进版本库，
删掉重新 build 就会再拉一次。锁定的版本记在 `dependencies.lock` 里。

改信道等组件配置见 [kconfig.md](kconfig.md)。

---

## `relay_switch`

**`examples/relay_switch`** — 最应该第一个读的示例。112 行，把这个库的核心思想全讲完了。

硬件：`PIN_RELAY = GPIO5` 继电器，`PIN_BUTTON = GPIO9` 按键（板载 BOOT 键）。
继电器是一路 GPIO 输出（内置 `driver`），按键用
[`espressif/button`](https://components.espressif.com/components/espressif/button)。

### 它演示的两条控制方向

```
远程：HA → 协调器 → CMD 帧 → en2m 任务
                               └→ on_write() → gpio_set_level()
                                               └→ 成功才提交 + 上报

本地：按键回调 → en2m_schedule(toggle)
                 └→ en2m 任务跑 toggle()
                     └→ en2m_attribute_get() 读当前值
                     └→ en2m_attribute_write(!current)
                         └→ on_write() → gpio_set_level()
                                         └→ 成功才提交 + 上报
```

**两条路在 `on_write` 汇合**。这是整个示例最值得抄的一点：

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_ON_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return gpio_set_level(PIN_RELAY, value->v.b == RELAY_ACTIVE_HIGH);
}
```

全固件只有这一处碰继电器。本地按键**没有**直接 `gpio_set_level`，
而是走 `en2m_attribute_write`。好处：

- 状态一定和组件里的一致，HA 里不会出现"开关显示关但灯亮着"
- 按键触发的变化一定会上报，不用自己记得调上报函数
- 继电器坏了（`gpio_set_level` 返回错误）时，本地和远程的失败行为一样：
  不提交、不上报、HA 里状态弹回

**为什么继电器不用注册表组件**：因为没有东西可抽象。
一个引脚、一个电平，`gpio_set_level()` 就是完整的驱动。
注册表里没有继电器组件不是遗漏。

### 按键回调的正确写法

```c
static void on_button(void *button_handle, void *usr_data)
{
    en2m_schedule(toggle, NULL);
}
```

注意这里是 `en2m_schedule` 而**不是** `en2m_schedule_from_isr`：
`espressif/button` 不挂 GPIO 中断，它在一个 esp_timer 上按
`CONFIG_BUTTON_PERIOD_TIME_MS`（默认 5 ms）轮询，
连续 `CONFIG_BUTTON_DEBOUNCE_TICKS`（默认 2）次读到同一电平才认。
所以回调跑在**任务上下文**，取锁、发包在技术上都是合法的。

**但还是要 `en2m_schedule`**，理由是那个 esp_timer 是
**全固件所有按键实例共用的一个**（`iot_button.c` 里的 `g_head_handle` 链表
被同一个定时器回调遍历）。在里面等锁、等射频发送，
会连带把同一块板上其它按键的消抖一起卡住。

**规矩：按键回调里只派活，不干活。**

（如果你换成自己写的 GPIO 中断，那就必须用 `en2m_schedule_from_isr()`
并处理 `higher_prio_task_woken`。真 ISR 里**不能**调
`en2m_attribute_get` / `en2m_attribute_write`，两个都要拿互斥锁。）

### `SINGLE_CLICK` 有 ~180 ms 延迟

示例注册的是 `BUTTON_SINGLE_CLICK`，组件要等 `short_press_time`
（默认 180 ms）过去、确认没有第二击才发这个事件。
墙面开关嫌慢的话改注册 `BUTTON_PRESS_DOWN`，按下瞬间就动，
代价是从此无法区分单击和双击。
状态机的完整解释见
[`scene_switch` 的手势时间参数](../examples/scene_switch/README.md#手势时间参数)。

### 持久化是白拿的

`EN2M_DEVICE_TYPE_ON_OFF_PLUG` 的 `OnOff` 属性默认 `persist = true`，所以：

- 每次继电器状态变化，5 秒内会批量写进 NVS
- 重启后 `en2m_start` 会把上次的值**通过 `on_write`** 写回继电器

正因为回放走 `on_write`，`relay_init()` 必须在 `en2m_start`
**之前**调用。示例里就是这个顺序。详见 [persistence.md](persistence.md)。

**继电器状态只有组件这一份持久化。** 应用侧不要再自己往 NVS 写一遍——
理由见[§外设组件](#外设组件)。

### 要改成你自己的设备

| 改什么 | 怎么改 |
|---|---|
| 引脚 | `PIN_RELAY` / `PIN_BUTTON` |
| 继电器是低电平有效 | `#define RELAY_ACTIVE_HIGH false` |
| 按键是高电平有效 | `#define BUTTON_ACTIVE_LEVEL 1` |
| 不是继电器而是 MOSFET/SSR | `on_write` 里换成你的输出函数就行 |
| 多路继电器 | 见 [usage.md 配方 E](usage.md#配方-e--一个固件驱动多个互不相关的外设)，用多 cluster |
| HA 里显示成灯而不是插座 | `EN2M_DEVICE_TYPE_ON_OFF_LIGHT` |

---

## `dimmable_light`

**`examples/dimmable_light`** — 一个回调覆盖三个 cluster，
输出是一条真的 WS2812 灯带
（[`espressif/led_strip`](https://components.espressif.com/components/espressif/led_strip)，
RMT 后端）。默认引脚 `GPIO8` 是 C3 DevKit 上板载的那颗灯珠，
所以不接线也能看到效果。

```c
switch (path->cluster_id) {
case EN2M_CLUSTER_ON_OFF:        s_light.on     = value->v.b;   break;
case EN2M_CLUSTER_LEVEL_CONTROL: s_light.level  = value->v.u8;  break;  /* 0..254 */
case EN2M_CLUSTER_COLOR_CONTROL: s_light.mireds = value->v.u16; break;
default: return ESP_ERR_NOT_SUPPORTED;
}
light_apply();
```

值得注意的几点：

- **先 `switch (cluster_id)`。** 这三个 cluster 的属性 ID 分别是 `0x0000`、
  `0x0000`、`0x0007`——前两个撞车了，只看 `attribute_id` 必然出错。
- **`light_apply()` 在 switch 之后统一调一次。** 三个参数里改了哪个都要重新
  算输出，这样写比在每个 case 里各调一次干净。
- 亮度是 **0–254**（Matter 的 `CurrentLevel` 范围），不是 0–100 也不是 0–255。
  上报到 HA 时组件会转成 0–255 的 `brightness`。
- 色温单位是 **mired**（`1e6 / 开尔文`），默认 300（≈3333 K）。
- 三个属性都是 `persist = true`，所以断电再来灯会恢复到原来的亮度和色温。

### `identify` 回调 + 手动加 cluster

```c
ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT);
en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);      /* 配方里不含 Identify */
```

这是全仓库唯一演示 Identify 的地方。设备类型配方**不会**自动加 Identify cluster，
必须自己 `en2m_cluster_create`。加了之后组件会每秒把 `IDENTIFY_TIME` 自减并回调
你的 `identify`，你只管闪灯。详见 [state-flow.md](state-flow.md#9-identify-倒计时)。

### mired → RGB：数据模型和硬件说的不是同一种语言

WS2812 的灯珠吃 RGB，Matter 的 Color Control 说的是**迈尔德**。
中间那一步是这个示例唯一有实质内容的代码：一张 10 点的黑体辐射表
（2000 K–6500 K，正好覆盖 HA 色温滑块的 154–500 迈尔德），线性插值。

红色分量全程 255，只有绿蓝在动——这不是表做得糙，
真实黑体在这个区间里就是这样："暖"就是压绿蓝，"冷"就是补回来。

### gamma 加在亮度上，不是加在通道上

```c
gamma_level = (uint32_t)s_light.level * s_light.level / 254u;
r = (uint8_t)((uint32_t)r * gamma_level / 254u);
g = (uint8_t)((uint32_t)g * gamma_level / 254u);
b = (uint8_t)((uint32_t)b * gamma_level / 254u);
```

WS2812 的占空比是线性的，人眼不是，所以亮度必须做 gamma
（这里用 γ≈2.0，一次乘法搞定，不用查表也不用浮点）。

**关键是先算 gamma、再整体缩放三个通道。**
如果反过来对 R/G/B 各自平方，通道之间的**比例**就变了
（255² : 141² ≠ 255 : 141），色温会跟着亮度飘：调暗一点颜色就偏冷。
先定比例再定总量，色温才稳。

这个坑不止 WS2812 有——任何 RGB 输出（RGB LEDC、LED 驱动 IC）都一样。

### 换成 LEDC 单色 / 双色温

把 `idf_component.yml` 里的 `espressif/led_strip` 删掉，改用内置 LEDC：

```c
static void light_apply(void)
{
    /* gamma 照样要做 */
    uint32_t total = s_light.on ? (uint32_t)s_light.level * s_light.level * 8191
                                      / (254 * 254)
                                : 0;
    uint32_t warm_pct = (s_light.mireds - 154) * 100 / (500 - 154);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_WARM, total * warm_pct / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_COLD, total * (100 - warm_pct) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_WARM);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_COLD);
}
```

`on_write` 一行都不用改——这就是把硬件关在一个函数里的好处。

**`light_apply()` 跑在 en2m 任务上**，所以别在里面做渐变循环。
要渐变就起一个 `esp_timer`，在定时器回调里推进一帧。

---

## `smart_plug`

**`examples/smart_plug`** — 同一个 endpoint 上，一个 cluster 走写、一个走读。

```c
cfg.attribute_write = on_write;   /* OnOff        → 继电器 */
cfg.attribute_read  = on_read;    /* 功率 / 电量  → 计量芯片 */
```

硬件和 `relay_switch` 相同（GPIO 继电器 + `espressif/button`）。
计量是唯一一处没有注册表组件可用的外设：
BL0937 / HLW8012 的接口就是一路脉冲，标定系数还得一块板一块板实测，
没什么可复用的，所以示例里是 `esp_random()` 的 stub。

这是最典型的"混合设备"结构。判断依据就是
[usage.md 第 3 步](usage.md#第-3-步给每个属性决定走哪条路)那张表：
继电器是执行器 → 写；功率是随时可读的传感器 → 读。

### 累计电量怎么做

示例里的 `meter_sample()` 演示了一个实际问题：**功率是瞬时值，电量是积分值**。

```c
static int32_t meter_sample(void)
{
    int64_t now = esp_timer_get_time();

    if (s_meter.last_sample_us != 0) {
        int64_t elapsed_us = now - s_meter.last_sample_us;
        s_meter.energy_mwh += (int64_t)s_meter.power_mw * elapsed_us / (3600LL * 1000000LL);
    }
    s_meter.last_sample_us = now;
    /* …读新的瞬时功率… */
}
```

**在读回调里积分**，用上一次的功率乘这段时间。因为读回调被调用的时机就是
"要上报了"，所以积分的时间片天然对齐上报周期。`report_interval_ms = 15000`
（示例里设的）意味着每 15 秒积一次分——够准，而且不需要任何额外的定时器。

注意：

- `EN2M_ATTR_ENERGY_MWH` 是 `int64_t` 且 `persist = true`，所以累计电量断电不丢。
  `EN2M_ATTR_ACTIVE_POWER_MW` 是 `int32_t`、不持久化——瞬时值存了没意义。
- 组件对同一个 cluster 里的每个属性**分别**调读回调，所以 `on_read` 里要
  `if (path->attribute_id == ...)` 分开处理。示例里功率那一支会顺手做积分，
  电量那一支只是取出来——这依赖"功率先被读"的顺序，实际上两个属性的顺序就是
  建的顺序（`ACTIVE_POWER_MW` 先，`ENERGY_MWH` 后），所以是对的。
- 换真芯片（BL0937 / HLW8012 / CSE7766）时只改 `meter_sample()`。BL0937 是脉冲
  输出，要用 PCNT 或者 GPIO 中断计数——计数放中断，换算放读回调。
  **这一路不要用 `espressif/button`**：它按 5 ms 轮询消抖，数不了几百赫兹的脉冲。
  按键组件是给"人按的开关"用的，不是计数器。
- `meter_sample()` 靠一个 `static bool s_relay_on` 知道继电器开没开，
  那份副本在 `on_write` 里更新。**不要在 `on_read` 里回头调
  `en2m_attribute_get`**：`on_read` 是组件在组帧过程中调的，
  数据模型的锁已经被持住了。需要什么状态就在写它的地方留一份副本。

---

## `th_sensor`

**`examples/th_sensor`** — 最纯粹的拉取型传感器，只有一个读回调。

传感器是 [`espressif/aht20`](https://components.espressif.com/components/espressif/aht20)
（I²C，SDA=GPIO4 / SCL=GPIO5）。
注册表里**没有 DHT 组件**，而 AHT20 正是 DHT22 的现代替代品：
同量程、更高精度、标准 I²C、不需要外挂上拉电阻。

```c
cfg.attribute_read = on_read;
cfg.report_interval_ms     = 60000;   /* 一分钟上报一次 */
cfg.min_report_interval_ms = 5000;    /* 顺手压住自热 */
```

### 为什么这么写就够了

组件在**每次组报文之前**遍历所有带读回调的属性，调一次回调拿新值。所以：

- 采样频率 = 上报频率，一次都不浪费
- 应用里没有定时器、没有任务
- AHT20 读得太勤会自热（芯片温度高于环境零点几度），
  靠 `min_report_interval_ms = 5000` 顺手兜住

这是"组件持有时序"最直白的体现。想改采样频率，改 `report_interval_ms`，别的都不用动。

### 一次测量供两个属性用

组件是**每个属性调一次**读回调，而 AHT20 一次转换同时给出温度和湿度。
两次各测一遍的话，一轮上报要等 160 ms，而且两个数还来自不同时刻。

所以 `sample()` 带一个 2 秒的时间戳缓存：

```c
static esp_err_t sample(void)
{
    int64_t now = esp_timer_get_time();

    if (s_sample.taken_us != 0 && now - s_sample.taken_us < SAMPLE_CACHE_MS * 1000LL) {
        return ESP_OK;                  /* 上一次的还新鲜 */
    }
    ESP_RETURN_ON_ERROR(aht20_read_temperature_humidity(...), TAG, "aht20 read failed");
    /* …存值和时间戳… */
}
```

同一轮里的第二次调用落在窗口内，直接吃缓存。两个好处：

1. 同一条上报里的温度和湿度来自**同一次**测量，时间上一致
2. **不依赖属性被遍历的顺序**——谁先被问到谁去测，另一个复用，
   两种顺序结果都一样

注意这一点和 `smart_plug` 的积分正好相反：那里**依赖**了
`ACTIVE_POWER_MW` 先于 `ENERGY_MWH`（按建的顺序），
这里则刻意做成顺序无关。能做到顺序无关就该做到。

窗口设 2 秒是因为上报最快 5 秒一次，所以缓存**绝不会跨轮复用**。
改 `min_report_interval_ms` 到 2 秒以下的话，要把这个窗口一起调小。

### 两个设备类型叠在一个 endpoint 上

```c
ep = en2m_endpoint_create(ENDPOINT);                                  /* 空 endpoint */
en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR);
en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_HUMIDITY_SENSOR);
```

`en2m_endpoint_create_device` 只能设一种类型，要叠加就先建空的再 `add`。
HA 那边会看到**两个** sensor 实体（温度、湿度），因为 HA 的实体是按 cluster
生成的，不是按 endpoint。

### `ESP_RETURN_ON_ERROR` 的用法

```c
ESP_RETURN_ON_ERROR(sample(), TAG, "temperature read failed");
```

I²C 偶尔会失败（线长、干扰）。这里返回错误（不是 `ESP_ERR_NOT_SUPPORTED`）的效果是
**保留上一次的缓存值继续上报**，HA 里不会出现空洞。如果返回
`ESP_ERR_NOT_SUPPORTED`，效果一样（保留缓存），区别只在语义：
`NOT_SUPPORTED` 说"这个属性不是我管的"，错误说"是我管的但这次没读到"。

单位是 **0.01 °C** 和 **0.01 %RH**（`en2m_i16` / `en2m_u16`）。
全部单位约定见 [data-model.md](data-model.md#单位约定)。

---

## `contact_sensor`

**`examples/contact_sensor`** — 推送为主 + 拉取兜底，这个组合值得抄。

干簧管在电路上就是一个按键：一根线、两个电平、会抖。
所以这里直接用
[`espressif/button`](https://components.espressif.com/components/espressif/button)，
把两个边沿都注册上：

```c
iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_contact);
iot_button_register_cb(s_contact, BUTTON_PRESS_DOWN, NULL, on_contact_edge, NULL);
iot_button_register_cb(s_contact, BUTTON_PRESS_UP,   NULL, on_contact_edge, NULL);
/* …建 endpoint… */
en2m_start(&cfg);                                  /* cfg.attribute_read = on_read */
```

消抖是白拿的（组件 5 ms 轮询、连续两次同电平才认，约 10 ms 窗口），
内部上拉也由 `active_level = 0` 自动打开。

### 为什么要两条路

| 路径 | 作用 | 延迟 |
|---|---|---|
| 边沿回调 → `en2m_schedule` → `en2m_report_boolean_state` | 门一开一关立刻上报 | 几十毫秒 |
| `attribute_read` | 每次周期上报前重读真实电平 | 最多一个上报周期 |

只有事件路径的话，**丢一帧就会永久卡住**——门开着但 HA 显示关着，直到下一次
开关。ESP-NOW 是无确认传输，丢包不是假设而是必然。
读回调是一个几乎免费的自愈机制：下一次周期上报会把真实电平重新同步回去。

这个模式适合所有"状态型"的数字传感器：门磁、水浸、按钮锁定、雨感。

### 回调里回读硬件

```c
static bool contact_is_open(void)
{
    return (iot_button_get_key_level(s_contact) == BUTTON_ACTIVE) == CONTACT_OPEN_WHEN_ACTIVE;
}

static void publish_contact(void *arg)      /* 在 en2m 任务上 */
{
    en2m_report_boolean_state(ENDPOINT, contact_is_open());
}

static void on_contact_edge(void *button_handle, void *usr_data)
{
    en2m_schedule(publish_contact, NULL);
}
```

两个细节：

**同一个回调挂两个事件。** 回调根本不看触发它的是 `PRESS_DOWN` 还是
`PRESS_UP`，它去回读电平。所以不需要两个函数，也不需要 `switch`。

**回读而不是取反。** "上次是 ON 那这次就是 OFF"的写法一旦错过一个边沿
就永久反相；回读则自动纠正。在**任务里**回读还顺手把抖动吃掉了：
一次机械抖动可能触发三四次回调，但等任务跑起来时电平已经稳定，
三四次都读到同一个值，而组件的属性去重会让它只上报一次。

`iot_button_get_key_level()` 就是一次 `gpio_get_level()`（见
`button_gpio.c` 的 `button_gpio_get_key_level`），不经过消抖状态机，
任何任务都能调，也没有延迟。

### 极性

`EN2M_CLUSTER_BOOLEAN_STATE` 的 `STATE_VALUE` 在 HA 里映射成 `contact` 键。
示例上报的是 `open`。极性拆成了**两个互相独立**的宏：

```c
#define CONTACT_ACTIVE_LEVEL 0        /* 电气极性：吸合时引脚是低还是高 */
#define CONTACT_OPEN_WHEN_ACTIVE true /* 语义极性：吸合算"门开"还是"门关" */
```

第一个取决于你接上拉还是下拉，第二个取决于你把磁铁装在哪。
这两件事没有关系，所以不该塞进一个开关。
接反了改宏，别在回调里取反——改配置比改逻辑好维护。

---

## `scene_switch`

**`examples/scene_switch`** — 唯一一个**一个回调都没注册**的示例，
因为按键是纯上行的：没有任何东西可以被远程写。

```c
en2m_device_config_t cfg = {
    .mesh = {.role = EN2M_ROLE_LEAF, .name = "switch1", .model = "ex-scene"},
    .report_mode = EN2M_REPORT_ON_CHANGE_ONLY,
};

en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_GENERIC_SWITCH);
en2m_start(&cfg);
button_init();                      /* ← 事件源在 en2m_start 之后 */
```

### 四种手势全部来自组件

短按 / 双击 / 长按 / 释放**不是手写的状态机**，是
[`espressif/button`](https://components.espressif.com/components/espressif/button)
的原生事件。应用侧只有一张映射表：

```c
static const struct {
    button_event_t event;
    en2m_press_action_t action;
} map[] = {
    {BUTTON_SINGLE_CLICK,     EN2M_PRESS_SHORT},
    {BUTTON_DOUBLE_CLICK,     EN2M_PRESS_DOUBLE},
    {BUTTON_LONG_PRESS_START, EN2M_PRESS_LONG},
    {BUTTON_LONG_PRESS_UP,    EN2M_PRESS_RELEASE},
};

for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    iot_button_register_cb(btn, map[i].event, NULL, on_press,
                           (void *)(uintptr_t)map[i].action);
}
```

**一个回调服务四个事件，而且回调里没有分支**——
要发哪个 action 是通过 `usr_data` 带进来的，原封不动传给
`en2m_schedule()` 的 `arg`：

```c
static void report_press(void *arg)         /* 在 en2m 任务上 */
{
    en2m_report_button(ENDPOINT, (en2m_press_action_t)(uintptr_t)arg);
}

static void on_press(void *button_handle, void *usr_data)
{
    en2m_schedule(report_press, usr_data);
}
```

为什么一定要经过 `en2m_schedule`：`en2m_report_button()` 内部是
"读计数 → 写 action → 写计数 +1"，**三步不是原子的**。
全部收拢到 en2m 任务上，计数就不可能被两次按键撕裂。
（这也正是 `en2m_report_button_from_isr` 的做法——它就是把参数打包
`en2m_schedule_from_isr` 出去。）

### `short_press_time` 不是"多短算短按"

```c
const button_config_t btn_cfg = {
    .long_press_time = LONG_PRESS_MS,  /* 800 ms */
    .short_press_time = CLICK_GAP_MS,  /* 300 ms */
};
```

这个字段的名字有误导性。看 `iot_button.c` 的状态机就清楚了：
`short_press_ticks` 用在 `PRESS_REPEAT_DOWN_CHECK` 状态，
也就是**松手之后等待下一击的窗口**。

```
按下 ──> PRESS_DOWN
松开 ──> PRESS_UP，开始计时
          ├─ 窗口内又按下  ──> repeat++，继续等
          └─ 窗口内没动作  ──> 按 repeat 发 SINGLE_CLICK / DOUBLE_CLICK
```

所以它同时决定两件事：**双击能有多慢**（超过窗口就变两次单击），
以及**单击有多慢**（必须等满窗口才能确定没有第二下）。
默认 180 ms 对不少人偏紧，示例放宽到 300 ms，代价是单击延迟 300 ms。

两个容易踩的地方：

- **不注册 `BUTTON_DOUBLE_CLICK` 也一样要等**，状态机是同一套。
  真要即时响应只能改用 `BUTTON_PRESS_DOWN`。
- **双击的第二下如果按住超过这个窗口**，状态机走到 `PRESS_END`
  而不发 `DOUBLE_CLICK`。"快按一下、再按住"什么都不会发。
  长按要单独用 `BUTTON_LONG_PRESS_START`，别指望它和连击混用。

HA 里会出现一个 `event.switch1_button` 实体，
四种事件类型：`press` / `double_press` / `long_press` / `release`。

### 为什么 `report_mode` 是 `ON_CHANGE_ONLY`

默认模式（`EN2M_REPORT_DEFAULT`）会额外发周期性保活报文。
按键节点没有任何值会自己变化，周期上报除了耗电什么也不做——
而这类设备通常是纸电池供电的。

`ON_CHANGE_ONLY` 的代价是**协调器的离线判定会误判**：
90 秒收不到帧就算离线，而一个没人按的开关可以几天不发一个字节。
真做电池开关的话要么接受"HA 里长期显示 unavailable"，
要么改回 `EN2M_REPORT_DEFAULT` 并把周期拉长到几分钟。

### 计数器不用自己管

`en2m_report_button()` 内部读出 `PRESS_COUNT` 加一再写回。
应用只说"按了哪一种"，不碰计数器。

原因是这个计数器**不是给人看的，是协议的一部分**：
`<slug>/state` 是 retained 的全量快照，所以两次短按会产生两条
一模一样的 payload，接收方分不清是按了两次还是同一条被重发。
计数器是唯一的区分手段。详见
[reporting.md](reporting.md#按键报的是计数器不是按了)。

### 换成真硬件

改 `PIN_BUTTON` 和 `BUTTON_ACTIVE_LEVEL` 就行。两个时间参数的手感建议：

| 参数 | 示例值 | 合理范围 |
|---|---|---|
| `LONG_PRESS_MS` | 800 | 600–1000，低于 500 容易和连击混 |
| `CLICK_GAP_MS` | 300 | 250–400 |

消抖不在应用里配，它是组件的全局 Kconfig
（`BUTTON_PERIOD_TIME_MS` 默认 5 ms × `BUTTON_DEBOUNCE_TICKS` 默认 2，
约 10 ms 窗口）。机械按键抖动通常 < 20 ms，默认够用；
SW-520D 这类振动开关能抖到 50 ms，要往上调。

### 做成多按键：开销几乎为零

四键场景开关的正确做法是一个按键一个 endpoint，
`usr_data` 里同时打包 endpoint 和 action：

```c
#define PACK(ep, action) ((void *)(uintptr_t)(((ep) << 8) | (action)))

static void report_press(void *arg)
{
    uintptr_t packed = (uintptr_t)arg;
    en2m_report_button((uint8_t)(packed >> 8), (en2m_press_action_t)(packed & 0xFF));
}
```

**`espressif/button` 的所有实例共用同一个 esp_timer**
（`iot_button.c` 里所有按键挂在 `g_head_handle` 链表上，
被同一个定时器回调遍历）。四个按键不是四个任务四个定时器，
就是同一个 5 ms 回调里多扫三个引脚。实例数量没有上限，只受内存限制。

这也正是为什么按键回调里绝对不能阻塞：**卡住一个就卡住全部**。

老版本这里是每个按键一个 2.5 KB 栈的任务，四个按键 10 KB。
换成组件之后这笔开销没有了，而且
**`scene_switch` 从"唯一用到任务的示例"变成了和其它示例一样干净**。

## `occupancy_sensor`

**`examples/occupancy_sensor`** — 一个 endpoint 上推、拉并存，
而且两路传感器各自演示了一个不同的"别阻塞 en2m 任务"的办法。

```c
cfg.attribute_read = on_read;    /* 光照：拉；人在：也走 read 兜底 */
/* 人在：推，PIR 的边沿回调 */
```

硬件：PIR 接 GPIO6（用
[`espressif/button`](https://components.espressif.com/components/espressif/button)），
BH1750 接 I²C（SDA=GPIO4 / SCL=GPIO5，
[`espressif/bh1750`](https://components.espressif.com/components/espressif/bh1750)）。

### PIR 为什么也是"按键"

一个 PIR 模块的输出就是一根会变电平的线：有人时拉高，保持时间到了拉低。
和干簧管、和墙面按键在电气上没有任何区别，所以用同一个组件，
和 `contact_sensor` 一样把两个边沿都注册上：

```c
iot_button_register_cb(s_pir, BUTTON_PRESS_DOWN, NULL, on_pir_edge, NULL);
iot_button_register_cb(s_pir, BUTTON_PRESS_UP,   NULL, on_pir_edge, NULL);
```

`PRESS_DOWN` = 有人了，`PRESS_UP` = 保持时间过了。
**两个都要注册**——只挂一个的话永远报不出另一边。
这正是自己写 GPIO 中断时最常犯的错（只配 `GPIO_INTR_POSEDGE`，
于是只报 `ON` 不报 `OFF`）；用组件的话两行摆在一起，很难写漏。

PIR 的输出级是推挽的，所以 `button_gpio_config_t` 里要
`.disable_pull = true`，否则内部上拉会和模块对抗。

### `en2m_schedule` 而不是 `_from_isr`

```c
static void on_pir_edge(void *button_handle, void *usr_data)
{
    en2m_schedule(publish_motion, NULL);
}
```

`espressif/button` 不挂 GPIO 中断，它在一个 esp_timer 上轮询，
回调跑在任务上下文，所以是不带 `_from_isr` 的那个。

技术上这里直接调 `en2m_report_occupancy()` 也能跑。绕一层
`en2m_schedule` 有两个理由：那个 esp_timer 是全固件按键共用的一个，
不能在里面等锁等发包；以及把所有数据模型操作收拢到一个任务上，
省掉一整类竞态。见 [concurrency.md](concurrency.md#4-每个回调跑在哪个上下文)。

### BH1750 跑连续模式，读回调才不会阻塞

```c
bh1750_power_on(s_bh1750);
bh1750_set_measure_mode(s_bh1750, BH1750_CONTINUE_1LX_RES);
```

BH1750 的**单次**高分辨率测量要 120 ms。
如果在 `on_read()` 里"触发测量 + 等结果"，就会把 en2m 任务卡住 120 ms，
那段时间收不了包——这正是 [concurrency.md](concurrency.md) 反复强调的事。

**连续模式**让器件自己一直测、结果常驻寄存器，
`on_read()` 里的 `bh1750_get_data()` 只是一次几毫秒的 I²C 读。
代价是静态电流从几 µA 涨到 120 µA，对常电设备完全无所谓。

这是和 `th_sensor` 不同的第二种解法。两种都值得记住：

| 办法 | 用在哪 | 适合 |
|---|---|---|
| 让器件连续测，读回调只取寄存器 | `occupancy_sensor` 的 BH1750 | 器件支持连续模式，且不在乎静态功耗 |
| 时间戳缓存，第一次调用去测 | `th_sensor` 的 AHT20 | 一次转换出多个值，或者器件只有单次模式 |
| `esp_timer` 周期采样，读回调只取缓存 | 采样 > ~100 ms 时 | DS18B20、要加热的气体传感器 |

电池设备要用 `BH1750_ONETIME_1LX_RES`，
那 120 ms 就必须挪出 `on_read()`，走上表第三行。

### 顺序：硬件在前，事件源在后

```c
ESP_ERROR_CHECK(light_sensor_init());   /* I²C 在 en2m_start 之前 */
/* …建 endpoint… */
ESP_ERROR_CHECK(en2m_start(&cfg));
ESP_ERROR_CHECK(pir_init());            /* 事件源在 en2m_start 之后 */
```

**通用规则：硬件初始化在 `en2m_start` 之前，事件源在之后。**

前半句是因为持久化状态的回放会在 `en2m_start` 里调你的写回调
（见 [`relay_switch`](#relay_switch)）；
后半句是因为 `en2m_schedule` 在队列建好之前会返回
`ESP_ERR_INVALID_STATE`，那次事件就丢了。

这个示例两边都有，是这条规则最完整的演示。
（`contact_sensor` 把按键初始化放在 `start` 前面，
那几毫秒里的边沿会被丢掉，但反正开机时门的状态会被第一次周期上报兜住。）

### 人在状态的自愈路径

`on_read` 里除了照度还处理了 `EN2M_CLUSTER_OCCUPANCY`：

```c
case EN2M_CLUSTER_OCCUPANCY:
    *out_value = en2m_bool(pir_is_active());
    return ESP_OK;
```

作用和 `contact_sensor` 的读回调完全一样：ESP-NOW 丢一帧，
HA 里的状态就会和现实不一致，有了这一段，下一个周期上报自动对齐，
不用等下一次有人经过。

### 换别的传感器

| 部分 | 换成 |
|---|---|
| PIR | 毫米波 LD2410 / LD2450（能测**静止**的人；UART，注册表里没有组件，要自己解析） |
| BH1750 | `espressif/veml6040`（可见光 + 色温）、`veml6075`（紫外） |

换成毫米波之后 `on_read(OCCUPANCY)` 的兜底要改成返回解析任务维护的那个变量，
不能再读 GPIO 了。

光照单位是 **lux**（`en2m_u32`）。PIR 通常自带几十秒的保持时间，
所以不需要在固件里做"无人延时"。

---

## `fan_controller`

**`examples/fan_controller`** — 唯一演示 `en2m_cluster_set_write_cb` 的示例。

```c
fan_cluster = en2m_cluster_get(ep, EN2M_CLUSTER_FAN_CONTROL);
ESP_ERROR_CHECK(en2m_cluster_set_write_cb(fan_cluster, on_fan_write, &s_fan));
/* 注意 cfg 里完全没有 .attribute_write */
```

### 为什么要按 cluster 注册

设备级回调的问题是：一个固件驱动三个外设时，你会得到一个 100 行的 `switch`，
而且所有外设共用一个 `user_ctx`，只能在里面再查一次表。

按 cluster 注册解决这两点：

```c
static esp_err_t on_fan_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    fan_ctx_t *fan = (fan_ctx_t *)ctx;       /* 直接就是我的上下文 */

    switch (path->attribute_id) {            /* cluster 已确定，只需看 attribute */
    case EN2M_ATTR_FAN_MODE:        fan->mode    = (en2m_fan_mode_t)value->v.e8; break;
    case EN2M_ATTR_PERCENT_SETTING: fan->percent = value->v.u8;                  break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    fan_apply(fan);
    return ESP_OK;
}
```

调用顺序是 **cluster 回调 → 设备级回调 → 直接提交**，任一级返回
`ESP_ERR_NOT_SUPPORTED` 就往下走。所以两种风格可以混用：特殊 cluster 用
cluster 回调，其余的用设备级回调兜住。
规则见 [callbacks.md](callbacks.md#6-三级-fall-through)。

### mode 和 percent 的关系

`FanControl` cluster 有两个属性，HA 会生成一个 fan 实体，既有档位又有百分比。
两者的一致性由**组件在设备侧**自动维护：内置的命令翻译在写完一个之后会顺手
修正另一个。

| HA 发来的命令 | 组件做的事 |
|---|---|
| `SET_FAN_PERCENT`，`percent = 0` | 写 `PERCENT_SETTING = 0`，然后如果 mode 不是 OFF，再写 `FAN_MODE = OFF` |
| `SET_FAN_PERCENT`，`percent > 0` | 写 `PERCENT_SETTING`，然后如果 mode 是 OFF，再写 `FAN_MODE = ON` |
| `SET_FAN_MODE = OFF` | 写 `FAN_MODE = OFF`，然后写 `PERCENT_SETTING = 0` |
| `SET_FAN_MODE = 其它` | 只写 `FAN_MODE` |
| `percent` 越界 | 夹到 0..100 |

**所以一条 HA 命令可能让你的写回调被调用两次**（先 percent 后 mode，或者反过来）。
这不是 bug，是在替你维持两个属性的一致。你的回调只需要各自记住自己那份，
`fan_apply` 里按 `mode == EN2M_FAN_OFF ? 0 : percent` 算实际输出即可——
示例就是这么写的，所以第二次回调进来时输出会被重算一遍，结果正确。

档位枚举：`OFF=0, LOW=1, MEDIUM=2, HIGH=3, ON=4, AUTO=5, SMART=6`。

### 换真硬件

`fan_apply` 里换成：

| 风机类型 | 输出 |
|---|---|
| PWM 直流 / EC 电机 | LEDC，`percent` 映射到占空比 |
| 可控硅调速 | 过零检测 + 触发延时，`percent` 映射到导通角 |
| 三档继电器 | 按 `mode` 选一路继电器，`percent` 分三段 |

---

## `window_cover`

**`examples/window_cover`** — 唯一只用 `command` 回调的示例，也是"慢执行器"的标准答案。

### 为什么不用写回调

窗帘电机走完全程要几秒到几十秒。如果用写回调：

```
HA 点"全关" → on_write(position=100) → 启动电机 → return ESP_OK
                                                  └→ 组件立刻提交 100 并上报
                                                     HA 显示已关闭，可是窗帘还在爬
```

用命令回调就正确了：

```
HA 点"全关" → on_command(DOWN_OR_CLOSE) → motor_go(100) → return ESP_OK
                                                          └→ 组件不碰位置属性
行程定时器每 200 ms → en2m_report_cover_position(实际位置)
                       └→ HA 里的百分比跟着真实位置走
到位 → motor_stop() → 最后报一次
```

`command` 回调返回 `ESP_OK` 表示**消费掉**这条命令，组件就不会再把它翻译成
属性写入。这是整个机制的关键。

### 四条命令都要实现

```c
case EN2M_CMD_UP_OR_OPEN:            motor_go(0);   return ESP_OK;
case EN2M_CMD_DOWN_OR_CLOSE:         motor_go(100); return ESP_OK;
case EN2M_CMD_GO_TO_LIFT_PERCENTAGE: motor_go((uint8_t)en2m_value_as_int(&cmd->arg)); return ESP_OK;
case EN2M_CMD_STOP_MOTION:           motor_stop();  return ESP_OK;
```

HA 的 cover 实体有开/关/停/百分比四个操作，少实现一个用户就会以为设备坏了。
特别是 **`STOP_MOTION`**——半开窗帘是很常见的需求。

> 如果 `command` 回调没实现 `STOP_MOTION`，组件会打
> `stop needs a command handler or a window covering driver`，
> 因为"停"没法翻译成一次属性写入。

### 位置约定

**0 = 全开，100 = 全闭**（和 Matter 一致，和某些国产协议相反）。
组件上报到 HA 时会把 `>= 95` 的位置额外标成 `"CLOSED"`，
理由见 [reporting.md](reporting.md#窗帘的-cover-键是推导出来的)。

`en2m_report_cover_position` 会把入参夹到 0..100，所以你的行程计算不用担心越界。

### 行程上报的限流

示例每 200 ms 走一步（`STEP_PERIOD_US`，每步 5%），也就是每 200 ms 调一次
`en2m_report_cover_position`。这不会刷网：`min_report_interval_ms` 默认 1000 ms
会自动限流，HA 那边看到的是每秒一次位置更新。想更平滑就把
`min_report_interval_ms` 调到 200–300。

### 换真硬件

`motor_step` / `motor_go` / `motor_stop` 换成你的电机控制。位置从哪来：

| 方案 | 位置来源 |
|---|---|
| 纯定时（最简单） | 标定一次全程秒数，按时间线性插值——就是示例的做法 |
| 霍尔 / 光电编码器 | 脉冲计数，PCNT 外设 |
| 限位开关 + 定时 | 定时插值，碰到限位就校准成 0 或 100 |
| 带位置反馈的电机模块 | 直接读 |

---

## `door_lock`

**`examples/door_lock`** — 最短的示例（45 行），专门演示持久化。

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_DOOR_LOCK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return bolt_drive(value->v.e8 == EN2M_LOCK_LOCKED);
}
```

### 开机恢复的完整链路

`EN2M_ATTR_LOCK_STATE` 的默认值是 `EN2M_LOCK_LOCKED`，`persist = true`。所以：

```
第一次开机     → NVS 里没有 → 用默认值 LOCKED → on_write(LOCKED) → bolt_drive(true)
HA 解锁        → on_write(UNLOCKED) → 提交 → 5 秒内写 NVS
断电重启       → en2m_start → en2m_dm_restore 读出 UNLOCKED
                             → on_write(UNLOCKED) → bolt_drive(false)
                             → 门锁保持在解锁状态，和断电前一致
```

注意**恢复是走 `on_write` 的**，不是直接改属性值。这样设计的理由：
硬件必须真的被驱动到那个状态，否则属性值和物理世界就不一致了。
代价是 `bolt_drive`（也就是硬件初始化）必须在 `en2m_start` 之前准备好。
完整分析见 [persistence.md](persistence.md#为什么用-write-而不是直接塞值)。

### 锁状态是枚举，不是布尔

```c
value->v.e8 == EN2M_LOCK_LOCKED      /* e8，不是 b */
```

`EN2M_VAL_ENUM8`，取 `v.e8`。`EN2M_LOCK_UNLOCKED = 0`，`EN2M_LOCK_LOCKED = 1`。
取错联合体成员在 C 里不会报错，但会读到垃圾——这类错误只能靠查
[data-model.md](data-model.md) 的属性表避免。

### 换真硬件

`bolt_drive` 换成你的执行机构。两个实际问题示例没处理，真做产品要补：

- **电磁锁不能长期通电**。`bolt_drive` 里应该起一个 `esp_timer` 断电，
  或者用双稳态电机锁。
- **要不要上报"实际"状态**。如果有霍尔传感器能测锁舌位置，应该用
  `en2m_report_lock_state` 上报实测值，而不是让写回调的成功返回代表到位。
  那就变成了 [配方 D](usage.md#配方-d--慢执行器窗帘--阀门--卷帘门) 的形状。

---

## `thermostat`

**`examples/thermostat`** — 三个回调配合，在设备上跑本地闭环。这是最复杂的示例。

```c
cfg.attribute_write   = on_write;             /* 模式和设定点从 HA 来 */
cfg.attribute_read    = on_read;              /* 室温从传感器来 */
cfg.attribute_changed = on_attribute_changed; /* 任何变化都重算一次 */
```

### 四个属性的分工

| 属性 | 类型 | 方向 | 持久化 |
|---|---|---|---|
| `SYSTEM_MODE` | enum8 | HA → 设备（写） | 是 |
| `OCCUPIED_HEATING_SETPOINT` | i16，0.01 °C | HA → 设备（写） | 是 |
| `OCCUPIED_COOLING_SETPOINT` | i16，0.01 °C | HA → 设备（写） | 是 |
| `LOCAL_TEMPERATURE` | i16，0.01 °C | 设备 → HA（读） | 否 |

三个"人设定的"持久化，一个"测出来的"不持久化。这就是
[persistence.md](persistence.md) 里那条规则的实例：**持久化只给执行器状态**。

### 闭环跑在哪

关键在 `hvac_control()` 被两处调用：

```c
/* 1) 写回调里：模式或设定点变了 */
static esp_err_t on_write(...)
{
    switch (path->attribute_id) { /* …更新 s_hvac… */ }
    hvac_control();
    return ESP_OK;
}

/* 2) changed 回调里：室温变了 */
static void on_attribute_changed(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->attribute_id == EN2M_ATTR_LOCAL_TEMPERATURE) {
        s_hvac.local_centi = value->v.i16;
        hvac_control();
    }
}
```

**没有控制任务，也没有控制定时器。** 闭环由事件驱动：

```
周期上报到点 → 组件调 on_read → 拿到新室温 → 提交
                                              └→ on_attribute_changed → hvac_control()
HA 改设定点 → 组件调 on_write → 更新设定点 → hvac_control()
```

室温的采样频率 = 上报频率。想让控温更灵敏就把 `report_interval_ms` 调小
（比如 30 秒），闭环频率跟着变——一个参数同时管住了两件事。

### 为什么 `LOCAL_TEMPERATURE` 既有读回调又在 changed 里处理

读回调返回的值被**提交**之后才触发 `changed`。所以 `on_read` 负责取值，
`on_attribute_changed` 负责响应。中间隔着组件的提交和去重：室温没变化时
`changed` 不会被调用，闭环也就不会白跑一次。

也可以在 `on_read` 里直接调 `hvac_control()`，但那样就丢掉了去重的好处，
而且 `on_read` 拿到的值还没提交，逻辑上更别扭。

### 回差（hysteresis）

```c
want = s_hvac.local_centi < s_hvac.heating_centi - 20;
if (s_hvac.local_centi > s_hvac.heating_centi + 20) {
    want = false;
}
```

`±20`（0.2 °C）的回差防止继电器在设定点附近来回跳。任何实际的温控都必须有这个，
示例里把它写进去了。真做产品还要加**最短开/关时间**（压缩机通常要求 3 分钟），
那个用 `esp_timer` 或者在 `hvac_control` 里记时间戳实现。

### `changed` 回调的重入陷阱

示例里 `hvac_control()` 只改本地变量和打日志，**没有**调 `en2m_attribute_set`。
这是有意的：`attribute_changed` 跑在触发提交的那个任务上，在它里面再提交属性
会**重入这个回调**。真要在闭环里上报点什么（比如一个"正在加热"的状态），
用 `en2m_schedule` 挪出去。见 [concurrency.md](concurrency.md#7-已知的并发注意点)。

### 上报的是哪个设定点

组件只上报**当前模式对应的**那个设定点：HEAT 模式报 heating，COOL 模式报 cooling。
HA 的 climate 实体只有一个 `temperature` 字段，所以这是必须的。
细节见 [reporting.md](reporting.md#温控器只报当前在追的那个-setpoint)。

---

## `firmware/router`

**`firmware/router`** — 不在 `examples/` 下，但它是最能说明分层的那个"示例"：
一个**只有传输层、没有数据模型**的固件，51 行。

```c
en2m_config_t cfg = {                       /* en2m_config_t，不是 en2m_device_config_t */
    .role  = EN2M_ROLE_ROUTER,
    .name  = "router1",
    .model = "ex-router",
    .fw    = EN2M_FW_VERSION,
};

ESP_ERROR_CHECK(en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m_event, NULL));
ESP_ERROR_CHECK(en2m_mesh_init(&cfg));      /* 不是 en2m_start */
```

### 它和设备固件的区别

| | 设备 | 路由器 |
|---|---|---|
| 启动函数 | `en2m_start(&device_cfg)` | `en2m_mesh_init(&mesh_cfg)` |
| 配置结构 | `en2m_device_config_t` | `en2m_config_t` |
| endpoint / cluster | 有 | **没有** |
| 回调 | 五个 | 没有（只有事件） |
| 上报 | 有 | 没有 |
| 转发 | 不转发（LEAF） | 上下行都转发 |
| 重播 beacon | 不 | 是，让更远的叶子能挂上来 |

`en2m_start` 内部就是"初始化数据模型 + 调 `en2m_mesh_init`"。路由器不需要
数据模型，所以直接调下层。这就是分层的实际价值：
**用不到的那一层可以整个不链接进来**（还能把三个容量宏压到最小，
见 [kconfig.md](kconfig.md#路由器节点转发量大)）。

S3 协调器（`espnow2mqtt-host` 仓库的 `firmware/coordinator`）用的是同一个模式：
`en2m_mesh_init` + `on_uplink` 回调，然后把帧转成 USB 上的 JSON。

### 纯事件驱动的可观测性

路由器没有属性可看，所以它的运行状况全靠事件：

```c
case EN2M_EVENT_PARENT_FOUND:  /* 挂上了谁，cost 多少，RSSI 多少 */
case EN2M_EVENT_PARENT_LOST:   /* 上行断了 */
case EN2M_EVENT_RX_DROPPED:    /* 队列扛不住了，该调大 EN2M_QUEUE_LEN */
```

`EN2M_EVENT_RX_DROPPED` 对路由器特别重要——它的转发量比叶子大一个数量级，
是最容易队列溢出的角色。部署路由器时一定要把这个事件接上，或者至少留着串口
看日志。12 个事件的完整清单见 [events.md](events.md)。

### 什么时候需要路由器

| 情况 | 要不要 |
|---|---|
| 所有设备离协调器一跳内（同层、无厚墙） | 不需要 |
| 隔一两层楼 / 隔承重墙 | 在中间放一个常电路由器 |
| 某几个设备 RSSI 长期低于 −80 | 就近加一个 |
| 设备很多（> 30）挤在一个协调器上 | 分区加路由器 |

路由器**必须常电**：它要一直收发 beacon 和转发，没法睡。电池设备一律用
`EN2M_ROLE_LEAF`。

---

## 外设组件

示例用到的外设驱动**一个都不在这个仓库里**，全部来自
[ESP 组件注册表](https://components.espressif.com)。
每个工程在自己的 `main/idf_component.yml` 里声明依赖，
`idf.py build` 下载到该工程的 `managed_components/`。

| 外设 | 组件 | 版本 | 用在 |
|---|---|---|---|
| WS2812 / SK6812 灯带 | [`espressif/led_strip`](https://components.espressif.com/components/espressif/led_strip) | `^3.0.3` | `dimmable_light` |
| 按键 | [`espressif/button`](https://components.espressif.com/components/espressif/button) | `^4.2.1` | `relay_switch`、`smart_plug`、`scene_switch` |
| 干簧管 / 门磁 | 同上 | `^4.2.1` | `contact_sensor` |
| PIR | 同上 | `^4.2.1` | `occupancy_sensor` |
| AHT20 温湿度 | [`espressif/aht20`](https://components.espressif.com/components/espressif/aht20) | `^2.0.0` | `th_sensor` |
| BH1750 照度 | [`espressif/bh1750`](https://components.espressif.com/components/espressif/bh1750) | `^2.0.0` | `occupancy_sensor` |
| 继电器 | 内置 `driver`（一路 GPIO 输出） | — | `relay_switch`、`smart_plug` |

### 为什么用注册表而不是自己写

自己写一遍 GPIO 消抖、一遍 WS2812 的 RMT 编码、一遍 DHT 的单总线时序，
写出来的东西**只在你自己的板子上验证过**。
注册表里的组件被几十万次下载验证过，有 CHANGELOG，
API 破坏性变更会升主版本号。
把它们当依赖声明出来，也让读者一眼看见这个示例到底依赖了什么。

### 三件推论

**一、干簧管、PIR、按键是同一个组件。**
这三样在电路上是同一种东西：一根线、两个电平、会抖。
`espressif/button` 已经做好消抖和手势识别，
再写一遍 GPIO 中断没有意义。

**二、继电器没有组件，因为没有东西可抽象。**
一个引脚、一个电平，`gpio_set_level()` 就是完整的驱动。
注册表里没有它不是遗漏。

顺便：老版本的 `drv_gpio_relay.c` 自己往 NVS 的 `"drv_relay"`
命名空间里写了一份状态，每次翻转刷一次盘。那是**第二个真相来源**，
和组件的属性持久化重复，还额外磨损 flash。删掉驱动顺手删掉了这个问题。

**三、注册表里没有 DHT 组件**，所以 `th_sensor` 用 AHT20。
这个替换是纯赚的：同量程、更高精度（±0.3 °C vs ±0.5 °C）、
标准 I²C 没有时序坑、不需要那颗必须外挂的上拉电阻。

### 一个要留意的不兼容

`espressif/aht20` 和 `espressif/sht3x` 走
[`espressif/i2c_bus`](https://components.espressif.com/components/espressif/i2c_bus)
封装（句柄类型 `i2c_bus_handle_t`），
而 `espressif/bh1750` 直接用 IDF 5.x 的 `driver/i2c_master.h`
（句柄类型 `i2c_master_bus_handle_t`）。

**两者不能共用一个 port 句柄。**
`th_sensor` 和 `occupancy_sensor` 是两个独立工程，所以互不影响；
但你要是想把 AHT20 和 BH1750 合到同一块板上，得先统一到一套 I²C API
——`i2c_bus_get_internal_bus_handle()` 可以从 `i2c_bus` 句柄里
掏出底层的 `i2c_master_bus_handle_t`，那是最省事的路。

引脚分配建议见 [wiring.md](wiring.md)。

# 示例逐个详解

示例在仓库根目录 **`examples/`**，一个目录一个独立 ESP-IDF 工程。
参考驱动在 **`drivers/`**，被多个示例共享。

十个示例不是十个"功能演示"，而是**十种把硬件接到这个库上的姿势**。选示例的时候
不要只看设备类型像不像，要看**回调组合**像不像——那个才是你要抄的东西。

## 一览

| 示例 | Cluster | 回调组合 | 演示的核心 |
|---|---|---|---|
| [`relay_switch`](#relay_switch) | OnOff | `write` + `changed` | 远程和本地走同一条路径；ISR → `en2m_schedule_from_isr` |
| [`dimmable_light`](#dimmable_light) | OnOff + Level + ColorControl | `write` + `identify` | 一个回调按 `cluster_id` 分发多个 cluster |
| [`smart_plug`](#smart_plug) | OnOff + ElectricalPower | `write` + `read` | 两种方向混用：执行器靠写，计量靠读 |
| [`th_sensor`](#th_sensor) | Temperature + Humidity | 只有 `read` | 纯拉取型；采样周期用 `min_report_interval_ms` 保护 |
| [`contact_sensor`](#contact_sensor) | BooleanState | `read`（兜底）+ ISR 推送 | 事件驱动 + 周期性自愈 |
| [`occupancy_sensor`](#occupancy_sensor) | Occupancy + Illuminance | `read` + `en2m_schedule` 推送 | 一个 endpoint 上推、拉两种传感器并存 |
| [`fan_controller`](#fan_controller) | FanControl | 按 cluster 注册的 `write` | `en2m_cluster_set_write_cb` + 私有 `ctx` |
| [`window_cover`](#window_cover) | WindowCovering | 只有 `command` | 慢执行器：命令只管启停，位置自己上报 |
| [`door_lock`](#door_lock) | DoorLock | 只有 `write` | 持久化：开机把上次状态写回执行器 |
| [`thermostat`](#thermostat) | Thermostat | `write` + `read` + `changed` | 三个回调配合跑设备端闭环 |
| [`firmware/router`](#firmwarerouter) | 无 | 只有事件 | 纯传输层：`en2m_mesh_init` 不带数据模型 |

**十个示例都没有 `while (1)`**，也都没有自建任务。

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

改信道等组件配置见 [kconfig.md](kconfig.md)。

---

## `relay_switch`

**`examples/relay_switch`** — 最应该第一个读的示例。79 行，把这个库的核心思想全讲完了。

硬件：`PIN_RELAY = GPIO5` 继电器，`PIN_BUTTON = GPIO9` 按键（板载 BOOT 键）。
用 `drivers/drv_gpio_relay.c` 和 `drivers/drv_gpio_button.c`。

### 它演示的两条控制方向

```
远程：HA → 协调器 → CMD 帧 → en2m 任务
                               └→ on_write() → drv_gpio_relay_set()
                                               └→ 成功才提交 + 上报

本地：按键 ISR → en2m_schedule_from_isr(toggle)
                 └→ en2m 任务跑 toggle()
                     └→ en2m_attribute_get() 读当前值
                     └→ en2m_attribute_write(!current)
                         └→ on_write() → drv_gpio_relay_set()
                                         └→ 成功才提交 + 上报
```

**两条路在 `on_write` 汇合**。这是整个示例最值得抄的一点：

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
        return drv_gpio_relay_set(value->v.b, ctx);
    }
    return ESP_ERR_NOT_SUPPORTED;
}
```

全固件只有这一处碰继电器。本地按键**没有**直接调 `drv_gpio_relay_set`，
而是走 `en2m_attribute_write`。好处：

- 状态一定和组件里的一致，HA 里不会出现"开关显示关但灯亮着"
- 按键触发的变化一定会上报，不用自己记得调上报函数
- 继电器坏了（`drv_gpio_relay_set` 返回错误）时，本地和远程的失败行为一样：
  不提交、不上报、HA 里状态弹回

### ISR 的正确写法

```c
static void on_button(void *ctx)              /* 在中断上下文 */
{
    BaseType_t woken = pdFALSE;
    en2m_schedule_from_isr(toggle, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}
```

ISR 里**不能**调 `en2m_attribute_get` / `en2m_attribute_write`（都要拿互斥锁）。
`en2m_schedule_from_isr` 把 `toggle` 挪到 `en2m` 任务，那里可以随便用 API。
`woken` / `portYIELD_FROM_ISR` 这一对是 FreeRTOS 的标准写法，不写也能工作，
写了响应更快。

### 持久化是白拿的

`EN2M_DEVICE_TYPE_ON_OFF_PLUG` 的 `OnOff` 属性默认 `persist = true`，所以：

- 每次继电器状态变化，5 秒内会批量写进 NVS
- 重启后 `en2m_start` 会把上次的值**通过 `on_write`** 写回继电器

正因为回放走 `on_write`，`drv_gpio_relay_init` 必须在 `en2m_start`
**之前**调用。示例里就是这个顺序。详见 [persistence.md](persistence.md)。

### 要改成你自己的设备

| 改什么 | 怎么改 |
|---|---|
| 引脚 | `PIN_RELAY` / `PIN_BUTTON` |
| 继电器是低电平有效 | `drv_gpio_relay_init(PIN_RELAY, false)` |
| 不是继电器而是 MOSFET/SSR | `on_write` 里换成你的输出函数就行 |
| 多路继电器 | 见 [usage.md 配方 E](usage.md#配方-e--一个固件驱动多个外设)，用多 cluster |
| HA 里显示成灯而不是插座 | `EN2M_DEVICE_TYPE_ON_OFF_LIGHT` |

---

## `dimmable_light`

**`examples/dimmable_light`** — 一个回调覆盖三个 cluster。

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

### 换成真硬件

示例里的 `light_apply()` 只打日志。换 LEDC：

```c
static void light_apply(void)
{
    uint32_t duty = s_light.on ? (s_light.level * 8191 / 254) : 0;   /* 13 bit */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
```

双色温灯要按 mired 在暖白/冷白两路之间分配占空比，`on_write` 一行都不用改。

---

## `smart_plug`

**`examples/smart_plug`** — 同一个 endpoint 上，一个 cluster 走写、一个走读。

```c
cfg.attribute_write = on_write;   /* OnOff        → 继电器 */
cfg.attribute_read  = on_read;    /* 功率 / 电量  → 计量芯片 */
```

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

---

## `th_sensor`

**`examples/th_sensor`** — 最纯粹的拉取型传感器，只有一个读回调。

```c
cfg.attribute_read = on_read;
cfg.report_interval_ms     = 60000;   /* 一分钟上报一次 */
cfg.min_report_interval_ms = 5000;    /* DHT22 最快 2 秒，留足余量 */
```

### 为什么这么写就够了

组件在**每次组报文之前**遍历所有带读回调的属性，调一次回调拿新值。所以：

- 采样频率 = 上报频率，一次都不浪费
- 应用里没有定时器、没有任务、没有缓存
- DHT22 有 2 秒最小采样间隔这个硬约束，直接靠 `min_report_interval_ms = 5000`
  兜住，不需要在驱动里再加节流

这是"组件持有时序"最直白的体现。想改采样频率，改 `report_interval_ms`，别的都不用动。

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
ESP_RETURN_ON_ERROR(drv_dht_get_temperature(&centi_celsius, ctx), TAG, "temperature read failed");
```

DHT 偶尔会校验失败。这里返回错误（不是 `ESP_ERR_NOT_SUPPORTED`）的效果是
**保留上一次的缓存值继续上报**，HA 里不会出现空洞。如果返回
`ESP_ERR_NOT_SUPPORTED`，效果一样（保留缓存），区别只在语义：
`NOT_SUPPORTED` 说"这个属性不是我管的"，错误说"是我管的但这次没读到"。

单位是 **0.01 °C** 和 **0.01 %RH**（`en2m_i16` / `en2m_u16`）。
全部单位约定见 [data-model.md](data-model.md#单位约定)。

---

## `contact_sensor`

**`examples/contact_sensor`** — 推送为主 + 拉取兜底，这个组合值得抄。

```c
drv_gpio_contact_init(PIN_CONTACT, CONTACT_ACTIVE_LOW);
/* …建 endpoint… */
drv_gpio_contact_watch(on_contact_edge, NULL);     /* 双边沿中断 */
en2m_start(&cfg);                                  /* cfg.attribute_read = on_read */
```

### 为什么要两条路

| 路径 | 作用 | 延迟 |
|---|---|---|
| GPIO 中断 → `en2m_schedule_from_isr` → `en2m_report_boolean_state` | 门一开一关立刻上报 | 毫秒级 |
| `attribute_read` | 每次周期上报前重读真实电平 | 最多一个上报周期 |

只有中断路径的话，**丢一次中断就会永久卡住**——门开着但 HA 显示关着，直到下一次
开关。读回调是一个几乎免费的自愈机制：下一次周期上报会把真实电平重新同步回去。

这个模式适合所有"状态型"的数字传感器：门磁、水浸、按钮锁定、雨感。

### 中断里回读硬件

```c
static void publish_contact(void *arg)      /* 在 en2m 任务上 */
{
    bool open = false;
    if (drv_gpio_contact_get(&open, NULL) == ESP_OK) {
        en2m_report_boolean_state(ENDPOINT, open);
    }
}

static void on_contact_edge(void *arg)      /* 在 ISR 上 */
{
    BaseType_t woken = pdFALSE;
    en2m_schedule_from_isr(publish_contact, NULL, &woken);
    if (woken) { portYIELD_FROM_ISR(); }
}
```

在**任务里**回读电平而不是在 ISR 里，顺手把抖动吃掉了：一次机械抖动可能触发
三四次中断，但等任务跑起来时电平已经稳定，三四次都读到同一个值，而组件的
属性去重会让它只上报一次。

如果你的事件源本身就带着值（不需要回读），可以更短：
用 `en2m_attribute_set_from_isr` 直接把值交给组件，连 `en2m_schedule` 都省掉。

### 极性

`EN2M_CLUSTER_BOOLEAN_STATE` 的 `STATE_VALUE` 在 HA 里映射成 `contact` 键。
示例上报的是 `open`（`drv_gpio_contact_get` 的语义），`CONTACT_ACTIVE_LOW = true`
表示磁铁靠近时引脚拉低。接反了就把 `CONTACT_ACTIVE_LOW` 改掉，别在回调里取反——
改配置比改逻辑好维护。

---

## `occupancy_sensor`

**`examples/occupancy_sensor`** — 一个 endpoint 上推、拉并存，而且演示了
用 `esp_timer` 当事件源。

```c
cfg.attribute_read = on_read;                 /* 光照：拉 */
/* 人体：推，由 esp_timer 模拟 PIR 中断 */
```

### `esp_timer` + `en2m_schedule` 的组合

```c
static void simulate_motion(void *arg)        /* esp_timer 任务上下文 */
{
    s_occupied = !s_occupied;
    en2m_schedule(publish_motion, NULL);      /* 挪到 en2m 任务 */
}
```

严格说，`esp_timer` 回调**不在** ISR 里（默认跑在 `esp_timer` 任务上），所以直接
调 `en2m_report_occupancy` 也能工作。示例故意多绕一层 `en2m_schedule`，是为了
演示"把工作统一收拢到 `en2m` 任务"的写法——换成真 PIR 的 GPIO 中断时，
只要把 `en2m_schedule` 换成 `en2m_schedule_from_isr`，其余一行不改。

> 这里有个容易忽略的细节：`esp_timer` 回调默认跑在专用任务上，但如果你创建 timer
> 时用了 `.dispatch_method = ESP_TIMER_ISR`，它就真的在中断里了，那时必须用
> `_from_isr` 版本。见 [concurrency.md](concurrency.md#回调的运行上下文)。

### 顺序：`en2m_start` 之后才启动事件源

```c
ESP_ERROR_CHECK(en2m_start(&cfg));
ESP_ERROR_CHECK(start_motion_simulation());   /* ← 在 start 之后 */
```

`en2m_schedule` 在组件启动前会失败（队列还不存在）。所以事件源要么在
`en2m_start` 之后启动（这个示例），要么容忍启动前的几次失败
（`contact_sensor` 把 `watch` 放在 `start` 前面，那几毫秒里的中断会被丢掉，
但反正开机时门的状态会被第一次周期上报兜住）。

### 换真硬件

| 部分 | 换成 |
|---|---|
| `simulate_motion` | PIR（HC-SR501 / AM312）的 GPIO 中断 |
| `on_read` 里的假光照 | BH1750 / TSL2591 的 I2C 读取 |

光照单位是 **lux**（`en2m_u32`）。PIR 通常自带几十秒的保持时间，所以不需要
在固件里做"无人延时"。

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
规则见 [callbacks.md](callbacks.md#三级-fall-through)。

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
理由见 [reporting.md](reporting.md#窗帘的关闭阈值)。

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
代价是 `bolt_drive`（这里是 `drv_*_init` 的位置）必须在 `en2m_start` 之前准备好。
完整分析见 [persistence.md](persistence.md#为什么恢复要走-write-而不是直接写值)。

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
用 `en2m_schedule` 挪出去。见 [concurrency.md](concurrency.md#已知的注意事项)。

### 上报的是哪个设定点

组件只上报**当前模式对应的**那个设定点：HEAT 模式报 heating，COOL 模式报 cooling。
HA 的 climate 实体只有一个 `temperature` 字段，所以这是必须的。
细节见 [reporting.md](reporting.md#温控器只报生效的设定点)。

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

## 参考驱动

`drivers/` 下四个驱动被示例共享，也可以直接拿去用。它们刻意写得很薄——
硬件归应用，这是分层约定的一部分。

| 驱动 | API | 用在 |
|---|---|---|
| `drv_gpio_relay` | `init(pin, active_high)` / `set(on, ctx)` / `get(&on, ctx)` | `relay_switch`、`smart_plug` |
| `drv_gpio_button` | `init(pin, active_low, debounce_ms, cb, arg)` | `relay_switch`、`smart_plug` |
| `drv_gpio_contact` | `init(pin, active_low)` / `get(&open, ctx)` / `watch(isr, arg)` | `contact_sensor` |
| `drv_dht` | `init(pin, type)` / `get_temperature(&centi_c, ctx)` / `get_humidity(&centi_pct, ctx)` | `th_sensor` |

两个约定值得注意：

- **所有 getter 都带一个 `void *ctx`**，即使当前实现忽略它。这样驱动可以在
  不改签名的情况下从"单实例"变成"多实例"，而回调里的 `ctx` 可以直接透传进去
  （`relay_switch` 里 `drv_gpio_relay_set(value->v.b, ctx)` 就是这么用的）。
- **`drv_gpio_button` 的回调在 ISR 上**，`debounce_ms` 是在驱动里用时间戳做的
  软消抖，不占定时器。

引脚分配建议见 [wiring.md](wiring.md)。

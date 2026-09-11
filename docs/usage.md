# 从零写一个设备

这篇是**动手指南**。如果你只想改现成示例，先看 [examples.md](examples.md)；
如果你想搞懂为什么是这样设计的，看 [architecture.md](architecture.md)；
如果你想知道某个回调的精确契约，看 [callbacks.md](callbacks.md)。

---

## 0. 先建立心智模型

写这个库的设备固件，最容易犯的错是**把它当成一个普通的 SDK 去轮询**。
它不是。它的分层和 ESP-Matter 一样：

```
              你写的代码
   ┌────────────────────────────────────┐
   │  硬件驱动（GPIO / I2C / LEDC …）    │   ← 你拥有硬件
   │  五个回调函数                       │   ← 你响应组件
   │  app_main（只跑一次，然后 return）   │   ← 你只做装配
   └────────────────────────────────────┘
        ↑ 回调              ↓ en2m_attribute_set / en2m_report_*
   ┌────────────────────────────────────┐
   │  en2m 组件                          │   ← 组件拥有状态、任务、时序
   │  数据模型 + 上报调度 + mesh + 重传    │
   └────────────────────────────────────┘
```

三条铁律，记住这三条就不会写错：

| 铁律 | 意思 |
|---|---|
| **组件持有状态** | 属性的当前值存在组件里，不要在应用里再存一份"真值"（驱动里的影子变量不算） |
| **组件持有任务** | 固件里**没有 `while(1)`**，`app_main` 装配完就 `return`；需要跑在任务上下文的活用 `en2m_schedule` |
| **组件持有时序** | 什么时候采样、什么时候上报、什么时候重传，都由组件决定；你不要自己定时上报 |

---

## 1. 五步法

### 第 1 步：建工程骨架

一个设备工程就是一个普通的 ESP-IDF 工程，只需要把 `components/` 指进来。

**`CMakeLists.txt`**（工程根）：

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_device)
```

> `EXTRA_COMPONENT_DIRS` 指向 `components/` 这个**目录**（不是 `components/en2m`）。
> 如果你把工程放在仓库外，改成绝对路径或者把 `components/en2m` 整个拷进自己工程的
> `components/` 下。

**`main/CMakeLists.txt`**：

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES en2m driver esp_timer
)
```

| `REQUIRES` 里的东西 | 什么时候需要 |
|---|---|
| `en2m` | 总是 |
| `driver` | 用了 GPIO / LEDC / I2C |
| `esp_timer` | 用了 `esp_timer_*` 做事件源或行程定时 |
| `nvs_flash` | **不需要**，`en2m` 自己 `REQUIRES` 了 |

**`sdkconfig.defaults`**：

```
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

`CONFIG_FREERTOS_HZ=1000` 不是可选项：组件的维护节拍是 100 ms，默认的 100 Hz
会让所有超时判断的精度掉到 10 ms 一跳，重传间隔也会变得很粗。

信道、超时、容量等 `en2m` 自己的配置项见 [kconfig.md](kconfig.md)。

### 第 2 步：选设备类型，建 endpoint

一次调用就够：

```c
#include "en2m.h"

#define ENDPOINT 1

en2m_endpoint_t *ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
if (ep == NULL) {
    ESP_LOGE(TAG, "could not create the endpoint");
    return;
}
```

`en2m_endpoint_create_device` = 建 endpoint + 把这个设备类型需要的 cluster 和
默认属性全都填好。17 种设备类型分别对应哪些 cluster，见
[data-model.md](data-model.md#5-设备类型配方)。

要**叠加**多个类型（比如温湿度二合一），先建空 endpoint 再加：

```c
en2m_endpoint_t *ep = en2m_endpoint_create(ENDPOINT);
ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR));
ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_HUMIDITY_SENSOR));
```

要**手搓**一个现成类型里没有的组合：

```c
en2m_endpoint_t  *ep = en2m_endpoint_create(ENDPOINT);
en2m_cluster_t   *c  = en2m_cluster_create(ep, EN2M_CLUSTER_ON_OFF);
/* en2m_cluster_create 已经建好了 well-known 属性，通常不用再手动建。
   只有在你要改默认值或者打开持久化时才需要： */
ESP_ERROR_CHECK(en2m_attribute_create(c, EN2M_ATTR_ON_OFF, en2m_bool(false), true));
```

> **必须在 `en2m_start` 之前**建完所有 endpoint / cluster / attribute。
> `en2m_start` 之后再建会返回 `ESP_ERR_INVALID_STATE`：属性表要在启动时一次性
> 从 NVS 回放，中途加进来的属性不会被恢复，`caps` 也已经算完了。

### 第 3 步：给每个属性决定走哪条路

这是整个设计里最需要想清楚的一步。对照下表：

| 这个属性是…… | 走哪条路 | 你要实现 | 组件会…… |
|---|---|---|---|
| 执行器，能立刻到位（继电器、LED、PWM） | **写回调** | `attribute_write` | 先叫你落硬件，成功才提交并上报 |
| 执行器，到位要时间（窗帘电机、阀门） | **命令回调** | `command` | 把命令交给你，你启停电机，位置由你自己 `en2m_report_*` |
| 传感器，随时可读（I2C / ADC / 一线） | **读回调** | `attribute_read` | 每次上报前叫你采一次样 |
| 传感器，事件驱动（门磁、PIR、按键计数） | **推送** | 在中断/定时器里调 `en2m_report_*` | 提交、通知、排一次上报 |
| 只读的统计量（累计电量） | **读回调**（在里面积分） | `attribute_read` | 同上 |
| 一个固件里好几个互不相关的外设 | **按 cluster 注册** | `en2m_cluster_set_*_cb` | 先试 cluster 回调，再试设备级回调 |

判断"能不能立刻到位"的标准很简单：**写回调返回的那一刻，硬件是不是已经在目标状态了？**
是 → 写回调。不是 → 命令回调。

搞错的后果很具体：用写回调驱动窗帘，HA 里的百分比会在你点下去的瞬间跳到 100%，
而窗帘还在慢慢爬——因为写回调一返回 `ESP_OK`，组件就认为到位了并上报。

### 第 4 步：实现回调

五个回调、每个的完整契约在 [callbacks.md](callbacks.md)。这里只给最小骨架：

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    switch (path->cluster_id) {              /* 先按 cluster 分发！ */
    case EN2M_CLUSTER_ON_OFF:
        return relay_set(value->v.b);
    case EN2M_CLUSTER_LEVEL_CONTROL:
        return pwm_set(value->v.u8);         /* 0..254 */
    default:
        return ESP_ERR_NOT_SUPPORTED;        /* 不是我的，交回给组件 */
    }
}
```

三个高频坑：

1. **一定先 `switch (path->cluster_id)`，再看 `attribute_id`。**
   九个不同 cluster 的主属性 ID 都是 `0x0000`（`EN2M_ATTR_ON_OFF`、
   `EN2M_ATTR_CURRENT_LEVEL`、`EN2M_ATTR_MEASURED_VALUE`…… 全都是 0）。
   只看 `attribute_id` 必然张冠李戴。完整清单见
   [data-model.md](data-model.md#注意-attribute-id-会重名)。
2. **不认识的路径返回 `ESP_ERR_NOT_SUPPORTED`，不要返回 `ESP_FAIL`。**
   `NOT_SUPPORTED` 的意思是"不是我管的，往下走"；`ESP_FAIL` 的意思是
   "我管，但我失败了"，组件会**拒绝提交**这次写入，HA 里的状态会弹回去。
3. **`value->v.xxx` 要取对联合体成员。** 类型是组件在建属性时定的，
   查 [data-model.md](data-model.md) 的属性表。不确定就用
   `en2m_value_as_int(value)`，它对任何类型都给一个 `int64_t` 视图。

### 第 5 步：填配置，`en2m_start`，收工

```c
void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {
            .role  = EN2M_ROLE_LEAF,     /* 电池设备用 LEAF；常电且要转发用 ROUTER */
            .name  = "relay1",           /* HA 里的实体名，≤31 字节 */
            .model = "my-switch",        /* 型号字符串，≤15 字节 */
        },
        .attribute_write   = on_write,
        .attribute_changed = on_changed, /* 可选：观察已提交的变化 */
        .user_ctx          = &s_dev,     /* 可选：回调里拿得到 */
        .report_interval_ms     = 30000, /* 可选：0 = 按角色取默认 */
        .min_report_interval_ms = 1000,  /* 可选：0 = 1000 ms */
    };

    /* 1) 先初始化硬件——en2m_start 会把持久化的状态写回执行器，
          那时驱动必须已经能用了。 */
    ESP_ERROR_CHECK(relay_init(PIN_RELAY));

    /* 2) 再建数据模型 */
    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG) == NULL) {
        return;
    }

    /* 3) 最后启动，app_main 到此结束 */
    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready");
}
```

**这个顺序是有意义的**，不能调换：

| 顺序 | 为什么 |
|---|---|
| 硬件驱动 **先于** `en2m_start` | 持久化属性的开机回放会调用你的 `attribute_write`，驱动没初始化就会失败，状态就丢了。详见 [persistence.md](persistence.md#5-开机恢复) |
| 数据模型 **先于** `en2m_start` | 启动后建模型返回 `ESP_ERR_INVALID_STATE` |
| 事件注册（可选）**先于** `en2m_start` | 否则会漏掉 `EN2M_EVENT_STARTED`。见 [events.md](events.md) |

`en2m_start` 之后 `app_main` 直接 `return` 就行——ESP-IDF 的 main 任务退出后，
组件自己的 `en2m` 任务会继续跑。

---

## 2. 四种配方

下面四段都是**能直接编译**的完整骨架，改改引脚和驱动就能用。

### 配方 A — 执行器（继电器 / 灯 / 插座）

特征：远程能控，本地按键也能控，断电后状态要恢复。

```c
#include "en2m.h"
#include "esp_log.h"
#include "drv_gpio_relay.h"
#include "drv_gpio_button.h"

#define ENDPOINT   1
#define PIN_RELAY  GPIO_NUM_5
#define PIN_BUTTON GPIO_NUM_9

static const char *TAG = "relay";

/* 全固件唯一一处碰继电器的地方 */
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
        return drv_gpio_relay_set(value->v.b, ctx);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/* 跑在 en2m 任务上，可以用全部 API */
static void toggle(void *arg)
{
    en2m_value_t cur;

    if (en2m_attribute_get(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, &cur) != ESP_OK) {
        return;
    }
    /* 走 write 而不是 set：和远程下发走完全同一条路径 */
    en2m_attribute_write(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!cur.v.b));
}

static void on_button(void *ctx)          /* 在 ISR 上下文 */
{
    BaseType_t woken = pdFALSE;
    en2m_schedule_from_isr(toggle, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "my-switch"},
        .attribute_write = on_write,
    };

    ESP_ERROR_CHECK(drv_gpio_relay_init(PIN_RELAY, true));
    ESP_ERROR_CHECK(drv_gpio_button_init(PIN_BUTTON, true, 40, on_button, NULL));

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG) == NULL) {
        return;
    }
    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

关键点：

- 本地按键**不直接调驱动**，而是走 `en2m_attribute_write`。这样本地和远程是
  同一条路径，状态一定一致，也一定会上报。
- ISR 里只能调 `*_from_isr` 系列。`en2m_schedule_from_isr` 把活挪到 `en2m` 任务，
  在那里可以随便用阻塞 API。上下文规则全表见
  [concurrency.md](concurrency.md#6-每个公开-api-的可调用上下文)。
- `EN2M_DEVICE_TYPE_ON_OFF_PLUG` 的 `OnOff` 属性默认 `persist = true`，
  所以重启后 `en2m_start` 会自动把上次的状态写回继电器。

### 配方 B — 拉取型传感器（I2C / 一线 / ADC）

特征：任何时候都能读，读一次有代价，希望"需要上报时才采样"。

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    switch (path->cluster_id) {
    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT: {
        int16_t centi_c = 0;
        ESP_RETURN_ON_ERROR(drv_dht_get_temperature(&centi_c, ctx), TAG, "temp");
        *out = en2m_i16(centi_c);        /* 0.01 °C */
        return ESP_OK;
    }
    case EN2M_CLUSTER_RELATIVE_HUMIDITY: {
        uint16_t centi_pct = 0;
        ESP_RETURN_ON_ERROR(drv_dht_get_humidity(&centi_pct, ctx), TAG, "hum");
        *out = en2m_u16(centi_pct);      /* 0.01 %RH */
        return ESP_OK;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;    /* 保留缓存值 */
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "th1", .model = "my-th"},
        .attribute_read = on_read,
        .report_interval_ms     = 60000,  /* 一分钟一次就够 */
        .min_report_interval_ms = 5000,   /* DHT22 最快 2 秒一次，留足余量 */
    };

    ESP_ERROR_CHECK(drv_dht_init(PIN_DHT, 22));

    ep = en2m_endpoint_create(ENDPOINT);
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR));
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_HUMIDITY_SENSOR));

    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

关键点：

- **`min_report_interval_ms` 就是你的采样保护间隔。** 读回调在上报前被调用，
  所以"最快多久上报一次"等于"最快多久采一次样"。传感器有最小采样周期时，
  把它设进去，不需要在驱动里再加一层节流。
- 读回调**跑在 `en2m` 任务上**，可以放心做阻塞 I2C。但不要超过一两百毫秒，
  否则会拖慢维护节拍（心跳、重传都在同一个任务上）。
- 返回错误（不是 `NOT_SUPPORTED`）时组件保留上一次的缓存值继续上报，
  不会把设备变成"无数据"。

### 配方 C — 推送型传感器（门磁 / PIR / 计数）

特征：状态自己变，变了就要马上上报。

```c
/* 兜底：周期上报前重读一次真实电平，能自愈"丢了一次中断"的卡死 */
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    bool open = false;

    if (path->cluster_id != EN2M_CLUSTER_BOOLEAN_STATE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (drv_gpio_contact_get(&open, ctx) != ESP_OK) {
        return ESP_FAIL;                 /* 保留缓存值 */
    }
    *out = en2m_bool(open);
    return ESP_OK;
}

/* 跑在 en2m 任务上：可以做阻塞读，可以用全部 API */
static void publish_contact(void *arg)
{
    bool open = false;

    if (drv_gpio_contact_get(&open, NULL) == ESP_OK) {
        en2m_report_boolean_state(ENDPOINT, open);
    }
}

static void on_contact_edge(void *arg)   /* ISR 上下文 */
{
    BaseType_t woken = pdFALSE;

    en2m_schedule_from_isr(publish_contact, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "door1", .model = "my-contact"},
        .attribute_read = on_read,
    };

    ESP_ERROR_CHECK(drv_gpio_contact_init(PIN_CONTACT, true /* active_low */));

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_CONTACT_SENSOR) == NULL) {
        return;
    }

    ESP_ERROR_CHECK(drv_gpio_contact_watch(on_contact_edge, NULL));
    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

如果电平本身就在 ISR 参数里、不需要回读硬件，可以更短一步到位——用
`en2m_attribute_set_from_isr` 直接把值交给组件，连 `en2m_schedule` 都省了：

```c
static void on_edge(void *arg)
{
    BaseType_t woken = pdFALSE;

    en2m_attribute_set_from_isr(ENDPOINT, EN2M_CLUSTER_BOOLEAN_STATE,
                                EN2M_ATTR_STATE_VALUE, en2m_bool(gpio_get_level(PIN) == 0),
                                &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}
```

事件源不是中断而是定时器或别的任务时，直接调便捷函数就行，不需要 `_from_isr`：

```c
static void on_pir_timer(void *arg)     /* esp_timer 任务上下文 */
{
    en2m_report_occupancy(ENDPOINT, pir_read());
}
```

关键点：

- 推送路径用 `en2m_attribute_set` / `en2m_report_*`，**不是** `en2m_attribute_write`。
  传感器的真值在应用手里，组件没有理由回头问你"能不能写"。两条路径的区别见
  [callbacks.md](architecture.md#45-两条属性访问路径故意不等价)。
- `en2m_report_boolean_state(ep, v)` 就是
  `en2m_attribute_set(ep, EN2M_CLUSTER_BOOLEAN_STATE, EN2M_ATTR_STATE_VALUE, en2m_bool(v))`
  的别名，16 个便捷函数一览见 [api-reference.md](api-reference.md#便利上报函数)。
- **值没变就不会上报。** 组件在属性存储层做去重，抖动的传感器不会刷网。
  真的想每次都发，用 `en2m_report_now()`。
- 同时挂一个 `attribute_read` 兜底是个好习惯：周期上报时重读一次真实电平，
  可以自愈"中断丢了一次导致状态卡住"。

### 配方 D — 慢执行器（窗帘 / 阀门 / 卷帘门）

特征：命令下来只是"开始动",到位要几秒甚至几十秒。

```c
static struct {
    uint8_t position;      /* 0 = 全开, 100 = 全闭 */
    uint8_t target;
    bool moving;
    esp_timer_handle_t timer;
} s_cover;

static void motor_stop(void)
{
    s_cover.moving = false;
    s_cover.target = s_cover.position;
    esp_timer_stop(s_cover.timer);
    en2m_report_cover_position(ENDPOINT, s_cover.position);   /* 报真实位置 */
}

static void motor_step(void *arg)          /* esp_timer 任务上下文 */
{
    /* …驱动一小步，更新 s_cover.position… */
    en2m_report_cover_position(ENDPOINT, s_cover.position);
    if (s_cover.position == s_cover.target) {
        motor_stop();
    }
}

static esp_err_t on_command(const en2m_command_t *cmd, void *ctx)
{
    if (cmd->cluster_id != EN2M_CLUSTER_WINDOW_COVERING) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    switch (cmd->id) {
    case EN2M_CMD_UP_OR_OPEN:             motor_go(0);   return ESP_OK;
    case EN2M_CMD_DOWN_OR_CLOSE:          motor_go(100); return ESP_OK;
    case EN2M_CMD_GO_TO_LIFT_PERCENTAGE:  motor_go((uint8_t)en2m_value_as_int(&cmd->arg)); return ESP_OK;
    case EN2M_CMD_STOP_MOTION:            motor_stop();  return ESP_OK;
    default:                              return ESP_ERR_NOT_SUPPORTED;
    }
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "cover1", .model = "my-cover"},
        .command = on_command,          /* 注意：没有 attribute_write */
    };
    /* …建 timer、建 endpoint… */
    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

关键点：

- **`command` 回调返回 `ESP_OK` 就消费掉了这条命令**，组件不会再把它翻译成属性写入，
  所以位置属性不会被提前改成目标值。
- 行程中反复调 `en2m_report_cover_position`，HA 里的百分比就会跟着真实位置走。
  `min_report_interval_ms`（默认 1000 ms）会自动把它限流，你不用自己节流。
- `en2m_report_cover_position` 会把入参夹到 0..100。
- `EN2M_CMD_STOP_MOTION` 一定要实现——HA 的 cover 实体有停止按钮，不实现会让用户
  以为设备卡了。

### 配方 E — 一个固件驱动多个互不相关的外设

不要写一个巨大的 `switch`。按 cluster 注册，每个 cluster 带自己的 `ctx`：

```c
typedef struct { const char *label; uint8_t percent; en2m_fan_mode_t mode; } fan_ctx_t;
static fan_ctx_t s_fan = {.label = "ceiling"};

static esp_err_t on_fan_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    fan_ctx_t *fan = (fan_ctx_t *)ctx;     /* 直接就是我的上下文，不用再查表 */

    switch (path->attribute_id) {          /* cluster 已经确定了，这里可以只看 attribute */
    case EN2M_ATTR_FAN_MODE:        fan->mode    = (en2m_fan_mode_t)value->v.e8; break;
    case EN2M_ATTR_PERCENT_SETTING: fan->percent = value->v.u8;                  break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    fan_apply(fan);
    return ESP_OK;
}

void app_main(void)
{
    en2m_endpoint_t *ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_FAN);
    en2m_cluster_t  *fan = en2m_cluster_get(ep, EN2M_CLUSTER_FAN_CONTROL);

    ESP_ERROR_CHECK(en2m_cluster_set_write_cb(fan, on_fan_write, &s_fan));
    /* 还有别的外设就再 en2m_cluster_get + en2m_cluster_set_write_cb 一次 */

    ESP_ERROR_CHECK(en2m_start(&cfg));     /* cfg 里可以完全不填 attribute_write */
}
```

调用顺序是 **cluster 回调 → 设备级回调 → 直接提交**，任何一级返回
`ESP_ERR_NOT_SUPPORTED` 就往下一级走。所以你可以用 cluster 回调处理特殊 cluster，
再用设备级回调兜住其余的。完整规则见 [callbacks.md](callbacks.md#6-三级-fall-through)。

> **多外设优先用多 cluster，而不是多 endpoint。** 当前的属性存储用的是
> 全局扁平键，上报时同一个 cluster 只会取编号最小的那个 endpoint，所以
> "两个 OnOff endpoint"不会在 HA 里变成两个开关。限制细节见
> [data-model.md](data-model.md#什么时候需要多个-endpoint)。

---

## 3. 常见模式

### 在设备上跑本地闭环

温控器这类设备需要"读到温度 → 决定加热还是停"。用 `attribute_changed` 做，
它在每次提交后被调用：

```c
static void on_changed(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    /* 温度变了、模式变了、设定点变了，都重新算一次 */
    if (path->cluster_id == EN2M_CLUSTER_THERMOSTAT) {
        thermostat_recompute();       /* 内部调 en2m_attribute_get 取其它属性 */
    }
}
```

`attribute_changed` **跑在触发写入的那个任务上**（远程命令是 `en2m` 任务，
本地 `en2m_attribute_set` 是你的任务），而且在它里面再调 `en2m_attribute_set`
会**重入**这个回调。要么自己防重入，要么把闭环用 `en2m_schedule` 挪出去。
见 [concurrency.md](concurrency.md#4-每个回调跑在哪个上下文)。

### 用事件做诊断，而不是轮询状态

```c
static void on_en2m_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case EN2M_EVENT_PARENT_FOUND: {
        const en2m_event_parent_t *p = data;
        ESP_LOGI(TAG, "parent " EN2M_MACSTR " rssi=%d hops=%u",
                 EN2M_MAC2STR(p->mac), p->rssi, p->hop_count);
        break;
    }
    case EN2M_EVENT_PARENT_LOST:
        ESP_LOGW(TAG, "lost the parent, searching");
        break;
    case EN2M_EVENT_RX_DROPPED:
        ESP_LOGW(TAG, "queue overflow, raise CONFIG_EN2M_QUEUE_LEN");
        break;
    }
}

/* 在 en2m_start 之前注册，才收得到 EN2M_EVENT_STARTED */
ESP_ERROR_CHECK(esp_event_handler_register(EN2M_EVENT, ESP_EVENT_ANY_ID, on_en2m_event, NULL));
```

12 个事件的 payload 和触发点见 [events.md](events.md)。

### 让 `identify` 真的能认设备

装十个一样的传感器之后，这个回调会救你一命。注意 `Identify` cluster
**不在任何设备类型配方里**，要自己加：

```c
static void on_identify(uint8_t endpoint_id, uint16_t seconds, void *ctx)
{
    led_blink_for(seconds);      /* 组件已经在替你倒计时属性，这里只管闪 */
}

/* app_main 里： */
en2m_endpoint_t *ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_DIMMABLE_LIGHT);
en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);   /* ← 这一行不能省 */
cfg.identify = on_identify;
```

### 只在工程里保留一份 en2m

`components/en2m` 在 device 和 host 两个仓库里是**逐字节相同**的两份 vendored 拷贝。
如果你要改组件本身，改完记得同步另一份，不然 S3 协调器和 C3 设备会跑在两个协议版本上。

---

## 4. 反模式

这一节列的都是真的有人会写、但在这个库里是错的写法。

| 反模式 | 为什么错 | 正确写法 |
|---|---|---|
| `while (1) { en2m_model_loop(); vTaskDelay(...); }` | `en2m_model_loop` 现在是空函数，组件自己有任务。这个循环只是白占一个任务和它的栈 | 删掉。`app_main` 直接 `return` |
| 自己建任务定时调 `en2m_report_now()` | 组件已经有周期上报了，你在和它抢空口 | 设 `report_interval_ms`，或 `EN2M_REPORT_MANUAL` + `en2m_report_schedule` |
| 本地按键直接调 `relay_set()` | 组件不知道状态变了，HA 里的开关和实际状态会不一致 | `en2m_attribute_write(...)`，让写回调去落硬件 |
| 在应用里存一份 `bool s_is_on` 当真值 | 两份状态一定会走偏，尤其在写失败的时候 | `en2m_attribute_get()` 读组件里的值；驱动里的影子变量只做硬件缓存 |
| 传感器用 `en2m_attribute_write` 上报 | 会去调你自己的写回调，绕一大圈还可能被自己拒绝 | `en2m_attribute_set` / `en2m_report_*` |
| 写回调里不认识的路径返回 `ESP_FAIL` | 组件会认为"该写但写失败了"，拒绝提交，HA 状态弹回 | 返回 `ESP_ERR_NOT_SUPPORTED` |
| 只 `switch (path->attribute_id)` | 九个 cluster 的主属性都是 `0x0000`，必然误判 | 先 `switch (path->cluster_id)` |
| 在 ISR 里调 `en2m_attribute_set` | 内部要拿互斥锁，会在中断里死掉 | `en2m_attribute_set_from_isr` |
| 在 ISR 里调 `en2m_report_now` / `en2m_attribute_write` / 任何 `en2m_report_*` | 同上 | `en2m_schedule_from_isr` 把活挪到任务上 |
| `en2m_start` 之后再 `en2m_endpoint_create` | 返回 `ESP_ERR_INVALID_STATE`，模型已经冻结 | 全部在 `en2m_start` 之前建 |
| `en2m_start` 之前不初始化硬件驱动 | 持久化状态回放会调写回调，驱动没准备好就丢状态 | 驱动 `init` 放在 `en2m_start` 前面 |
| 给传感器属性打开 `persist` | 白写 NVS、磨损 flash，开机还会先报一个过期的读数 | `persist` 只给执行器状态（开关、亮度、设定点、锁） |
| `min_report_interval_ms = 0` 以为能"实时" | 0 表示取默认 1000 ms，不是"不限流"；真要更快就填 50、100 | 填具体数值，并想清楚空口负载 |
| `user_ctx` 指向 `app_main` 里的局部变量 | `app_main` 返回后栈被回收,回调里拿到野指针 | 用 `static` 变量或 `malloc` |
| 从别的任务调 `en2m_report_now()` 并期待"立即发出" | 从非 `en2m` 任务调用时它只是排一次上报请求 | 想立刻发就在 `en2m_schedule` 的回调里调 |
| 电池设备设 `EN2M_ROLE_ROUTER` | Router 要一直收发 beacon 和转发，没法睡 | 电池设备一律 `EN2M_ROLE_LEAF` |
| 不同节点配不同 `CONFIG_EN2M_WIFI_CHANNEL` | ESP-NOW 不跨信道，设备永远找不到父节点 | 全网同一个信道，见 [kconfig.md](kconfig.md) |

---

## 5. 可以直接拷走的完整模板

一个什么都不接、只有一个开关的最小设备，`main.c` 全文：

```c
#include "en2m.h"
#include "esp_log.h"

#define ENDPOINT 1

static const char *TAG = "my_device";
static bool s_output;                 /* 硬件影子状态，只在这里读写 */

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_ON_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_output = value->v.b;
    ESP_LOGI(TAG, "output -> %s", s_output ? "on" : "off");
    /* 换成 gpio_set_level / ledc_set_duty；失败就 return ESP_FAIL */
    return ESP_OK;
}

static void on_changed(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    ESP_LOGI(TAG, "committed %u/%04x/%04x = %lld",
             path->endpoint_id, path->cluster_id, path->attribute_id,
             (long long)en2m_value_as_int(value));
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "my_device1", .model = "tmpl-1"},
        .attribute_write   = on_write,
        .attribute_changed = on_changed,
    };

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready");
}
```

烧进去之后应该看到这几行（组件本身很安静，只在关键节点说话）：

```
I (612)  en2m:      mesh init
I (615)  my_device: ready
I (1843) en2m:      parent=aa:bb:cc:dd:ee:ff cost=1 rssi=-42
```

`parent=` 那行出现就说明入网成功了。它迟迟不出现、或者反复出现 `parent stale`，
照 [troubleshooting.md](troubleshooting.md) 排；想不靠看日志判断，监听
`EN2M_EVENT_PARENT_FOUND` / `EN2M_EVENT_PARENT_LOST`，见 [events.md](events.md)。

> 这些是组件的默认日志。填了 `cfg.mesh.on_log` 之后它们会改走你的回调，
> `ESP_LOGI` 就不再打了。

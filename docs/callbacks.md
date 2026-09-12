# 回调契约

`en2m` 是回调驱动的。整个应用侧接口就是这五个函数指针，
外加可选的按 cluster 覆盖。

```c
typedef struct {
    en2m_config_t mesh;                          /* 传输配置 */

    en2m_attribute_write_cb_t   attribute_write;   /* 执行器：把值落到硬件 */
    en2m_attribute_read_cb_t    attribute_read;    /* 拉式传感器：采一个新值 */
    en2m_attribute_changed_cb_t attribute_changed; /* 观察者：值变了 */
    en2m_command_handler_t      command;           /* 自己实现的命令 */
    en2m_identify_cb_t          identify;          /* 认人效果 */
    void *user_ctx;                                /* 传给以上全部 */

    en2m_report_mode_t report_mode;
    uint32_t report_interval_ms;
    uint32_t min_report_interval_ms;
    uint32_t task_stack_size;
    uint8_t  task_priority;
} en2m_device_config_t;
```

一个设备通常只需要其中**一到两个**。对照表：

| 设备 | 需要的回调 |
|---|---|
| 继电器 / 开关 / 插座 | `attribute_write` |
| 调光灯 / 色温灯 | `attribute_write` |
| I2C / 一线传感器（温湿度、光照、气压） | `attribute_read` |
| 门磁 / PIR / 按键（中断驱动） | 不需要回调，直接 `en2m_attribute_set_from_isr` |
| 计量插座 | `attribute_write` + `attribute_read` |
| 窗帘 / 卷帘（有行程） | `command` |
| 温控器（带本地闭环） | `attribute_write` + `attribute_read` + `attribute_changed` |
| 一个固件驱动多个互不相关外设 | 每个 cluster 各注册 `en2m_cluster_set_*_cb` |

---

## 1. `attribute_write` — 把值落到硬件

```c
typedef esp_err_t (*en2m_attribute_write_cb_t)(const en2m_attr_path_t *path,
                                               const en2m_value_t *value,
                                               void *ctx);
```

### 什么时候被调

任何 `en2m_attribute_write()` 都会调它，包括：

| 触发来源 |
|---|
| HA / 协调器下发的命令（内建翻译成属性写） |
| `en2m_start` 时的持久化状态回放 |
| 你自己调 `en2m_attribute_write()`（物理按键、场景、定时器） |

**注意 `en2m_attribute_set()` 不会调它**。传感器的真相源在应用手里，
再穿一遍 write 回调是无意义的（还会无限递归）。

### 契约

| 返回值 | 含义 |
|---|---|
| `ESP_OK` | 硬件已经变成 `value`。库随即提交属性并安排上报 |
| `ESP_ERR_NOT_SUPPORTED` | "我不管这个路径"，交给下一级处理器 |
| 其他 `esp_err_t` | 硬件拒绝了。**属性不提交、不上报**，打一条 WARN |

最后一条是"绝不上报没到达的状态"的保证。一个卡住的继电器返回 `ESP_FAIL`，
HA 里看到的就还是旧状态，而不是一个谎。

### `value` 已经被类型强制过

进回调的 `value` 已经是属性声明的类型，可以直接访问对应的联合体成员：

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    switch (path->cluster_id) {
    case EN2M_CLUSTER_ON_OFF:
        return relay_set(value->v.b);          /* 声明为 bool，直接取 .b */
    case EN2M_CLUSTER_LEVEL_CONTROL:
        return pwm_set_duty(value->v.u8);      /* 声明为 u8 */
    case EN2M_CLUSTER_COLOR_CONTROL:
        return cct_set_mireds(value->v.u16);   /* 声明为 u16 */
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
```

不确定类型时用 `en2m_value_as_int(value)`。

### 上下文

跑在**调用 `en2m_attribute_write` 的那个任务**上。远程命令走 en2m 任务，
`en2m_start` 的回放走调 `en2m_start` 的任务（通常是 `main`）。
所以回调**必须是可重入安全的**，而且不能假设自己在某个特定任务上。
细节见 [concurrency.md](concurrency.md#4-每个回调跑在哪个上下文)。

### 不要在里面调 `en2m_attribute_set`

```c
/* ✗ 反模式 */
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    relay_set(value->v.b);
    en2m_attribute_set(path->endpoint_id, path->cluster_id, path->attribute_id, *value);
    return ESP_OK;      /* 库紧接着又会提交一次，多余 */
}
```

返回 `ESP_OK` 就够了，提交是库的事。

---

## 2. `attribute_read` — 采一个新值

```c
typedef esp_err_t (*en2m_attribute_read_cb_t)(const en2m_attr_path_t *path,
                                              en2m_value_t *out_value,
                                              void *ctx);
```

### 什么时候被调

**每次上报之前**，由 `en2m_dm_refresh()` 遍历所有属性时调用。
不是每个 tick，而是每次真的要发报文之前。所以**采样频率 = 上报频率**，
由 `report_interval_ms` 和 `min_report_interval_ms` 决定。

进来时 `*out_value` 是**当前缓存值**，可以参考它做增量计算。

### 契约

| 返回值 | 含义 |
|---|---|
| `ESP_OK` | `*out_value` 是新读数，库用 `en2m_attribute_set` 提交（值没变就静默） |
| `ESP_ERR_NOT_SUPPORTED` | 这个路径我不负责（或者这次读失败），**保留缓存值** |
| 其他 | 和 `NOT_SUPPORTED` 一样被忽略，缓存值保留 |

读失败时返回 `ESP_ERR_NOT_SUPPORTED` 而不是编一个假值——HA 会继续显示
上次的有效读数，比显示 `0 °C` 好。

```c
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    float t, h;
    uint32_t t_raw, h_raw;

    if (path->cluster_id != EN2M_CLUSTER_TEMPERATURE_MEASUREMENT &&
        path->cluster_id != EN2M_CLUSTER_RELATIVE_HUMIDITY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (aht20_read_temperature_humidity(s_aht20, &t_raw, &t, &h_raw, &h) != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;      /* 保留上次的好读数 */
    }
    *out = (path->cluster_id == EN2M_CLUSTER_TEMPERATURE_MEASUREMENT)
               ? en2m_i16((int16_t)(t * 100))
               : en2m_u16((uint16_t)(h * 100));
    return ESP_OK;
}
```

### 会被调很多次

`en2m_dm_refresh` 遍历**每一个已建的属性**。一个温湿度设备有两个属性，
所以每轮上报你的回调会被调两次，路径不同。

上面那种写法对同一个传感器**读了两遍**，而 AHT20 一次转换本来就同时给出
温度和湿度。两遍不只是浪费 80 ms，还意味着同一条上报里的两个数
来自不同时刻。加一层时间戳缓存就解决了：

```c
static struct { int64_t at_ms; float t, h; } s_cache;

static bool sample_if_stale(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    uint32_t t_raw, h_raw;

    if (s_cache.at_ms != 0 && now - s_cache.at_ms < 2000) {
        return true;                        /* 缓存还新鲜，复用 */
    }
    if (aht20_read_temperature_humidity(s_aht20, &t_raw, &s_cache.t,
                                        &h_raw, &s_cache.h) != ESP_OK) {
        return false;
    }
    s_cache.at_ms = now;
    return true;
}
```

顺带一个好处：这样写**不依赖属性被遍历的顺序**。谁先被问到谁去测，
另一个复用缓存，两种顺序结果都一样。
`th_sensor` 示例就是这么做的。

### 上下文

**总是 en2m 任务**，而且在锁外。所以可以阻塞、可以走 I2C、可以做一线时序。
但别超过一两百毫秒，否则收包和重传都会延后。

---

## 3. `attribute_changed` — 值变了

```c
typedef void (*en2m_attribute_changed_cb_t)(const en2m_attr_path_t *path,
                                            const en2m_value_t *value,
                                            void *ctx);
```

### 什么时候被调

**任何属性真的变了之后**，无论是 `set` 还是 `write` 路径。
"真的变了"的意思是新旧值不等——值没变就不会调。

没有返回值，纯观察者。库不在乎你做了什么。

### 典型用途

| 用途 | 例子 |
|---|---|
| 打日志 | `ESP_LOGI(TAG, "relay -> %s", value->v.b ? "on" : "off")` |
| 驱动本地指示 | 状态 LED、OLED 刷新 |
| 本地控制闭环 | 温控器：目标温度或当前温度一变，就重算继电器该不该开 |

`examples/thermostat` 的闭环就在这里：

```c
static void on_changed(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_THERMOSTAT) {
        control_loop();        /* 读几个属性，决定加热器开关 */
    }
}
```

### 上下文与重入

跑在**调用 `set`/`write` 的任务**上。在里面调 `en2m_attribute_set()`
写**另一个**属性是允许且常见的（就是上面那个闭环），但：

- 不要写回**同一个**属性，会递归。去重机制通常会切断循环，但别依赖
- 不要阻塞。它在写路径的关键路上，会拖慢命令响应
- 不要假设自己在 en2m 任务上（见 [concurrency.md](concurrency.md)）

### 和事件的区别

`EN2M_EVENT_ATTRIBUTE_UPDATED` 携带同样的信息，但：

| | `attribute_changed` 回调 | `ATTRIBUTE_UPDATED` 事件 |
|---|---|---|
| 上下文 | 调用者任务，同步 | 事件循环任务，异步 |
| 数量 | 一个 | 任意多个订阅者 |
| 时机 | 提交后立刻 | 稍后 |
| 适合 | 需要立即反应的本地控制 | 日志、统计、松耦合的模块 |

---

## 4. `command` — 自己实现命令

```c
typedef esp_err_t (*en2m_command_handler_t)(const en2m_command_t *command, void *ctx);
```

```c
typedef struct {
    uint8_t  endpoint_id;     /* 目标 endpoint，已解析（0 不会出现） */
    uint16_t cluster_id;      /* 目标 cluster */
    en2m_command_id_t id;     /* 解码后的命令 */
    const char *name;         /* 收到的原始命令名，**永不为 NULL** */
    en2m_value_t arg;         /* 主参数，没有时是 EN2M_VAL_NULL */
    uint16_t transaction_id;  /* 帧的 cmd_id，会被自动 ACK 回去 */
    const char *json;         /* 原始 payload，取自定义字段用 */
} en2m_command_t;
```

### 什么时候被调

收到下行命令帧时，**在内建翻译之前**。也就是你有机会先截下来自己处理。

### 契约

| 返回值 | 含义 |
|---|---|
| `ESP_OK` | 我处理了，库不再做内建翻译 |
| `ESP_ERR_NOT_SUPPORTED` | 我不管，交给下一级（设备级回调 → 内建翻译） |
| 其他 | 处理失败，库**不做**内建翻译，把错误往上传 |

### 什么时候真的需要它

大多数执行器用 `attribute_write` 就够了，因为库会把命令翻译成属性写。
需要 `command` 的情况只有一种：**目标状态不是立刻可达的**。

窗帘就是典型：收到"关闭"时电机开始转，但位置要几秒才到 100。
如果用 `attribute_write`，库会立刻把位置提交成 100 并上报——
HA 里显示已关好，实际还在动。所以窗帘用 `command`：

```c
static esp_err_t on_command(const en2m_command_t *command, void *ctx)
{
    if (command->cluster_id != EN2M_CLUSTER_WINDOW_COVERING) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    switch (command->id) {
    case EN2M_CMD_UP_OR_OPEN:
        motor_go(0);              /* 只是启动电机 */
        return ESP_OK;
    case EN2M_CMD_DOWN_OR_CLOSE:
        motor_go(100);
        return ESP_OK;
    case EN2M_CMD_GO_TO_LIFT_PERCENTAGE:
        motor_go((uint8_t)en2m_value_as_int(&command->arg));
        return ESP_OK;
    case EN2M_CMD_STOP_MOTION:
        motor_stop();
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
```

然后由行程定时器用 `en2m_report_cover_position()` 持续上报**真实位置**。
完整代码见 `examples/window_cover`。

`EN2M_CMD_STOP_MOTION` **没有内建实现**（库不知道电机停在哪），
所以窗帘设备必须实现 `command` 回调，否则 stop 会打 WARN 并返回 `NOT_SUPPORTED`。

### 用 `json` 字段取自定义参数

`command->json` 是原始 payload 字符串，可以自己再 parse 一次拿库不认识的字段：

```c
cJSON *doc = cJSON_Parse(command->json);
const cJSON *speed = cJSON_GetObjectItem(doc, "my_speed");
if (cJSON_IsNumber(speed)) { ... }
cJSON_Delete(doc);
```

### 上下文

**总是 en2m 任务**。可以阻塞、可以调任意 en2m API。

---

## 5. `identify` — 认人效果

```c
typedef void (*en2m_identify_cb_t)(uint8_t endpoint_id, uint16_t seconds, void *ctx);
```

### 什么时候被调

1. 收到 `IDENTIFY` 命令时，`seconds` = 请求的秒数
2. 之后**每秒一次**，`seconds` 递减，直到 0

`seconds == 0` 表示倒计时结束，**该停止效果**。

```c
static void on_identify(uint8_t endpoint_id, uint16_t seconds, void *ctx)
{
    if (seconds == 0) {
        led_set(s_light.on);        /* 恢复正常状态 */
    } else {
        led_set(seconds % 2);       /* 1 Hz 闪 */
    }
}
```

### 倒计时需要 Identify cluster

每秒递减是由 `en2m_model_identify_tick()` 驱动的，而它读的是
`IDENTIFY_TIME` 属性。**设备类型配方不会自动建 Identify cluster**，要自己建：

```c
en2m_endpoint_t *ep = en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_DIMMABLE_LIGHT);
en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);
```

没建的话回调仍会被调用一次（命令那一步），但不会有倒计时。

### 触发方式

```json
{"identify": 10}
{"ep":1,"cluster":"identify","command":"identify","seconds":10}
```

`seconds` 缺省是 10。

---

## 6. 三级 fall-through

写和命令都支持逐级下沉，用 `ESP_ERR_NOT_SUPPORTED` 表示"交给下一级"。

### 写路径

```
en2m_attribute_write(ep, cluster, attr, value)
    │
    ├─ ① cluster->write_cb          （en2m_cluster_set_write_cb 注册，带自己的 ctx）
    │      返回 NOT_SUPPORTED ↓
    ├─ ② cfg.attribute_write        （设备级，带 cfg.user_ctx）
    │      返回 NOT_SUPPORTED ↓
    └─ ③ 裸提交                      （没人管 = 直接接受）
```

### 命令路径

```
命令帧
    │
    ├─ ① cluster->command_cb        （en2m_cluster_set_command_cb 注册）
    │      返回 NOT_SUPPORTED ↓
    ├─ ② cfg.command                （设备级）
    │      返回 NOT_SUPPORTED ↓
    └─ ③ 内建翻译成属性写            （见 state-flow.md 的内建翻译表）
```

### 读路径（只有两级，没有内建）

```
en2m_dm_refresh 遍历到某个属性
    │
    ├─ ① cluster->read_cb 存在就用它 + read_ctx
    └─ ② 否则用 cfg.attribute_read + cfg.user_ctx
```

注意读路径是**"有就用"**而不是"失败就下沉"：只要 cluster 注册了 `read_cb`，
设备级的 `attribute_read` 就**不会**被这个 cluster 调用。和写/命令的
fall-through 语义不同。

### 什么时候用 cluster 级回调

一个固件驱动多个互不相关的外设时，cluster 级回调让每块逻辑带自己的状态：

```c
typedef struct { int pin; uint8_t last; } fan_ctx_t;
static fan_ctx_t s_fan = {.pin = 6};

void app_main(void)
{
    en2m_endpoint_t *ep = en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_FAN);
    en2m_cluster_t *fan = en2m_cluster_get(ep, EN2M_CLUSTER_FAN_CONTROL);

    en2m_cluster_set_write_cb(fan, fan_write, &s_fan);   /* 自己的 ctx */
    /* ... */
}

static esp_err_t fan_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    fan_ctx_t *fan = ctx;                  /* 直接拿到，不用查全局 */
    /* ... */
}
```

比一个大 switch 干净，而且加一个外设不用改别人的代码。
`examples/fan_controller` 演示了这种写法。

---

## 7. `user_ctx` 怎么用

`cfg.user_ctx` 会原样传给**全部五个**设备级回调。cluster 级回调用的是
注册时给的那个 ctx，互不干扰。

```c
typedef struct {
    int relay_pin;
    int64_t energy_mwh;
    int64_t last_sample_us;
} plug_ctx_t;

static plug_ctx_t s_plug = {.relay_pin = 5};

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = { ... },
        .attribute_write = on_write,
        .attribute_read  = on_read,
        .user_ctx = &s_plug,          /* 两个回调都拿到它 */
    };
    ...
}
```

`user_ctx` 必须在整个运行期有效——**不要指向栈上的东西**。
`app_main` 返回后它的栈就没了。用 `static` 或者堆。

---

## 8. 完整例子：一个回调管三个 cluster

`examples/dimmable_light` 的核心，一个 write 回调 + 一个 identify 回调
撑起一个完整的色温灯：

```c
static struct { bool on; uint8_t level; uint16_t mireds; } s_light;

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    switch (path->cluster_id) {
    case EN2M_CLUSTER_ON_OFF:
        s_light.on = value->v.b;
        break;
    case EN2M_CLUSTER_LEVEL_CONTROL:
        s_light.level = value->v.u8;
        break;
    case EN2M_CLUSTER_COLOR_CONTROL:
        s_light.mireds = value->v.u16;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    return apply_to_pwm(&s_light);        /* 一处落硬件 */
}

static void on_identify(uint8_t endpoint_id, uint16_t seconds, void *ctx)
{
    /* seconds == 0 时恢复 */
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "light1", .model = "ex-cct"},
        .attribute_write = on_write,
        .identify = on_identify,
    };
    en2m_endpoint_t *ep = en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT);
    if (ep == NULL) {
        return;
    }
    en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);
    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

HA 里的 `light` 实体的开关、亮度、色温**全部**走这一个回调。
开机时 NVS 里存的 on/level/mireds 也会通过同一个回调回放进 PWM，
所以灯会自己恢复到断电前的亮度。

---

## 相关文档

- 每个回调跑在哪个任务上 → [concurrency.md](concurrency.md#4-每个回调跑在哪个上下文)
- 命令怎么解码、内建翻译做了什么 → [state-flow.md](state-flow.md#4-命令下行流水线)
- 从零写一个设备的完整步骤 → [usage.md](usage.md)
- 10 个示例分别演示哪个特性 → [examples.md](examples.md)

# 从 driver-ops 版本迁移

`0.4.0-idf` 把 `en2m` 从 **driver-ops**（应用往组件里塞一堆函数指针结构体）
改成了 **回调式**（组件回调应用的五个函数）。这是一次**破坏性改动**，
没有兼容层可以让旧固件不改就跑起来。

好消息是改动是机械的，一个典型示例从 68 行缩到 45 行，而且改完之后**应用里的
任务和循环可以整个删掉**。

- 想直接看新写法 → [usage.md](usage.md)
- 想搞懂为什么这么改 → [architecture.md](architecture.md#4-关键设计决策)

---

## 1. 五分钟版：改什么

| 旧 | 新 |
|---|---|
| 15 个 `en2m_*_driver_t` 结构体 | 5 个回调函数 |
| `en2m_endpoint_add_on_off(ep, &driver)` | `en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG)` |
| `en2m_model_start(&mesh_cfg)` | `en2m_start(&device_cfg)` |
| `while (1) { en2m_model_loop(); … }` | **删掉**，组件有自己的任务 |
| `xTaskCreate(app_task, …)` | **删掉** |
| `en2m_model_notify(1, cluster, true)` | `en2m_attribute_set()` / `en2m_report_*()` |
| `en2m_model_report()` | `en2m_report_now()` |
| 轮询按键 | GPIO 中断 + `en2m_schedule_from_isr` |
| `nvs_flash_init()` | **删掉**，组件自己会初始化 |
| 驱动 getter 被组件按需调用 | `attribute_read` 回调，或者应用主动 `en2m_attribute_set` |
| 驱动 setter 被组件调用 | `attribute_write` 回调 |
| 属性状态存在驱动里 | **属性状态存在组件里**，断电由 NVS 恢复 |

---

## 2. 完整的前后对比

同一个继电器开关示例。

### 旧（68 行）

```c
static void app_task(void *arg)
{
    int last_btn = 1;
    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&in);

    while (1) {
        int btn = gpio_get_level(PIN_BUTTON);
        en2m_model_loop();                            /* ← 应用负责泵组件 */
        if (last_btn == 1 && btn == 0) {
            s_relay_on = !s_relay_on;
            gpio_set_level(PIN_RELAY, s_relay_on);    /* ← 应用直接改硬件 */
            en2m_model_notify(1, EN2M_CLUSTER_ON_OFF, true);  /* ← 再告诉组件 */
            vTaskDelay(pdMS_TO_TICKS(40));            /* ← 手写消抖 */
        }
        last_btn = btn;
        vTaskDelay(pdMS_TO_TICKS(20));                /* ← 20 ms 轮询 */
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_config_t mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"};

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(relay_init());

    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_on_off(ep, &(en2m_on_off_driver_t){
                                                     .set = relay_set,
                                                     .get = relay_get,
                                                 }));

    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "sw", 6144, NULL, 4, NULL);  /* ← 第二个任务 */
}
```

### 新（45 行，含注释）

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_ON_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return gpio_set_level(PIN_RELAY, value->v.b == RELAY_ACTIVE_HIGH);
}

static void toggle(void *arg)                          /* 跑在 en2m 任务上 */
{
    en2m_value_t current;

    if (en2m_attribute_get(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, &current) != ESP_OK) {
        return;
    }
    en2m_attribute_write(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!current.v.b));
}

static void on_button(void *button_handle, void *usr_data)   /* 任务上下文 */
{
    en2m_schedule(toggle, NULL);
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"},
        .attribute_write = on_write,
    };

    ESP_ERROR_CHECK(relay_init());                     /* gpio_config */
    ESP_ERROR_CHECK(button_init());                    /* espressif/button */

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG) == NULL) {
        return;
    }
    ESP_ERROR_CHECK(en2m_start(&cfg));                 /* app_main 到此返回 */
}
```

顺手拿到的四样东西，旧版本都没有：

1. **按键状态一定会上报**，因为它走 `en2m_attribute_write` → `on_write`，
   和远程下发同一条路径；旧版本是"先改硬件，再记得通知组件"，忘一次就不一致。
2. **继电器失败时不会谎报状态**。`on_write` 返回非 `ESP_OK` 时组件拒绝提交。
   旧版本的 `notify` 无条件上报。
3. **开关状态断电不丢**，走 NVS，见 [persistence.md](persistence.md)。
4. **少一个任务**（6144 字节栈）和少一次 20 ms 轮询。

---

## 3. 逐个 driver-ops 的替换

15 个 `en2m_endpoint_add_*` 全部删除了。对照下表改：

### 执行器类（旧 setter → `attribute_write`）

| 旧 | 新 |
|---|---|
| `en2m_endpoint_add_on_off(ep, {.set, .get})` | `en2m_cluster_create(ep, EN2M_CLUSTER_ON_OFF)`；`.set` → `attribute_write` 的 `EN2M_CLUSTER_ON_OFF` 分支取 `value->v.b` |
| `en2m_endpoint_add_level_control(ep, {.set_level, .get_level})` | `EN2M_CLUSTER_LEVEL_CONTROL`；取 `value->v.u8`（0–254） |
| `en2m_endpoint_add_color_control(ep, {.set_color_temp, …})` | `EN2M_CLUSTER_COLOR_CONTROL`；取 `value->v.u16`（mired） |
| `en2m_endpoint_add_fan_control(ep, {.set_mode, .set_percent, …})` | `EN2M_CLUSTER_FAN_CONTROL`；按 `path->attribute_id` 分 `FAN_MODE`（`v.e8`）和 `PERCENT_SETTING`（`v.u8`） |
| `en2m_endpoint_add_thermostat(ep, {.set_system_mode, .set_occupied_heating, …})` | `EN2M_CLUSTER_THERMOSTAT`；按 `attribute_id` 分 `SYSTEM_MODE`（`v.e8`）、两个 `SETPOINT`（`v.i16`） |
| `en2m_endpoint_add_door_lock(ep, {.lock, .unlock, .get_locked})` | `EN2M_CLUSTER_DOOR_LOCK`；`value->v.e8 == EN2M_LOCK_LOCKED` 判断锁还是开 |

**两个 `.lock` / `.unlock` 合成了一个写回调**——这是回调式最明显的收敛：
动作变成了"把属性写到某个值"，而不是"调某个动作函数"。

### 传感器类（旧 getter → `attribute_read`，或者主动 `en2m_attribute_set`）

| 旧 | 新（拉取型：`attribute_read`） | 新（推送型） |
|---|---|---|
| `add_boolean_state({.get})` | `EN2M_CLUSTER_BOOLEAN_STATE` → `*out = en2m_bool(v)` | `en2m_report_boolean_state(ep, v)` |
| `add_occupancy({.get_occupied})` | `EN2M_CLUSTER_OCCUPANCY` → `en2m_bool` | `en2m_report_occupancy(ep, v)` |
| `add_illuminance({.get_lux})` | `EN2M_CLUSTER_ILLUMINANCE` → `en2m_u32(lux)` | `en2m_report_illuminance(ep, lux)` |
| `add_temperature({.get_measured_value})` | `EN2M_CLUSTER_TEMPERATURE_MEASUREMENT` → `en2m_i16(centi_c)` | `en2m_report_temperature(ep, centi_c)` |
| `add_humidity({.get_measured_value})` | `EN2M_CLUSTER_RELATIVE_HUMIDITY` → `en2m_u16(centi_pct)` | `en2m_report_humidity(ep, centi_pct)` |
| `add_pressure({.get_hpa})` | `EN2M_CLUSTER_PRESSURE_MEASUREMENT` → `en2m_i32(deci_hpa)` | `en2m_report_pressure(ep, deci_hpa)` |
| `add_smoke_co({.get_smoke, .get_co})` | `EN2M_CLUSTER_SMOKE_CO`，按 `attribute_id` 分 `SMOKE_STATE` / `CO_STATE` | `en2m_report_smoke()` / `en2m_report_co()` |
| `add_electrical_power({.get_active_power, .get_energy})` | `EN2M_CLUSTER_ELECTRICAL_POWER`，按 `attribute_id` 分 `ACTIVE_POWER_MW`（`en2m_i32`）/ `ENERGY_MWH`（`en2m_i64`） | `en2m_report_power()` / `en2m_report_energy()` |

**旧的每个传感器 getter 都变成了读回调里的一个 `case`。** 迁移时最省事的做法是
把旧的 getter 原封不动留着，只是在读回调里调它们：

```c
/* 旧的 driver getter 一行不改 */
static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out, void *ctx)
{
    switch (path->cluster_id) {
    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT: {
        int16_t centi = 0;
        ESP_RETURN_ON_ERROR(my_old_get_measured_value(&centi, ctx), TAG, "temp");
        *out = en2m_i16(centi);
        return ESP_OK;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
```

事件驱动的传感器（门磁、PIR）应该趁这次改成**推送**：删掉 getter，
在中断里 `en2m_schedule_from_isr` + `en2m_report_*`。这样上报延迟从
"一个上报周期"降到毫秒级。

### 窗帘：从 `command` ops 到 `command` 回调

旧的窗帘 ops 是唯一一个"动作型"接口：

```c
/* 旧 */
esp_err_t (*command)(en2m_cover_command_t cmd, uint8_t position, void *ctx);
esp_err_t (*get_position)(uint8_t *position, void *ctx);
```

新的是 `command` 回调，命令枚举也换了名字：

| 旧 `en2m_cover_command_t` | 新 `en2m_command_id_t` |
|---|---|
| `EN2M_COVER_OPEN` | `EN2M_CMD_UP_OR_OPEN` |
| `EN2M_COVER_CLOSE` | `EN2M_CMD_DOWN_OR_CLOSE` |
| `EN2M_COVER_STOP` | `EN2M_CMD_STOP_MOTION` |
| `EN2M_COVER_GOTO` | `EN2M_CMD_GO_TO_LIFT_PERCENTAGE`（目标在 `cmd->arg`） |

`en2m_cover_command_t` 这个类型**已经删掉了**。

`get_position` 没有直接对应物，改成**行程中主动**
`en2m_report_cover_position(ep, pos)`。这比旧的 getter 更准：旧版本只在上报时
问一次位置，新版本可以在电机每走一步都报一次，HA 里的动画是跟着真实位置走的。
完整写法见 [examples.md](examples.md#window_cover)。

---

## 4. 改动清单（按顺序做）

### 第 1 步：删掉应用任务

```c
- static void app_task(void *arg) { while (1) { en2m_model_loop(); … } }
- xTaskCreate(app_task, "sw", 6144, NULL, 4, NULL);
```

`en2m_model_loop()` 和 `en2m_mesh_loop()` 现在是空函数（第一次被调用时会打一条
WARN 提醒你），留着只是为了让旧代码编得过。**删掉整个任务**，不要只删循环体。

任务里如果还有别的活（不是轮询、不是泵组件），搬到：

| 那个活是 | 搬到 |
|---|---|
| 响应 GPIO 变化 | GPIO 中断 + `en2m_schedule_from_isr` |
| 定时做某事 | `esp_timer` + `en2m_schedule` |
| 采样传感器 | `attribute_read` 回调，删掉定时 |
| 本地控制逻辑 | `attribute_changed` 回调 |
| 真的需要一直跑（比如驱动步进电机） | 保留自己的任务，但只碰硬件，通过 `en2m_report_*` 和组件交互 |

### 第 2 步：删掉 `nvs_flash_init()`

组件在 `en2m_start` 里自己初始化 NVS（并处理 `ESP_ERR_NVS_NO_FREE_PAGES`
需要 erase 的情况）。重复调用不会出错，但没必要留着。

如果你的应用**也**用 NVS 存自己的东西，保留 `nvs_flash_init()` 没问题——
它是幂等的。但组件用的是自己的命名空间 `"en2m_attr"`，不会和你冲突。

### 第 3 步：把 `add_*(ep, &driver)` 换成设备类型

```c
- ep = en2m_endpoint_create(1);
- en2m_endpoint_add_on_off(ep, &(en2m_on_off_driver_t){.set = …, .get = …});
+ en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
```

17 种设备类型和它们各自包含的 cluster 见
[data-model.md](data-model.md#5-设备类型配方)。找不到完全对应的类型就用
`en2m_endpoint_create` + `en2m_cluster_create` 自己拼，或者
`en2m_endpoint_add_device_type` 叠加多个。

> 旧版本的 `EN2M_MAX_ENDPOINTS` 是硬编码的 `4`，现在是
> `CONFIG_EN2M_MAX_ENDPOINTS`（默认仍然 4）。顺便可以把它和另外两个容量宏
> 调小省 RAM，见 [kconfig.md](kconfig.md#7-数据模型容量)。

### 第 4 步：把 ops 拼成回调

按上面第 3 节的表把每个 setter / getter 变成 `attribute_write` /
`attribute_read` 里的一个 `case`。

**最容易出错的一步**：旧的 ops 是按 cluster 分开注册的，所以每个函数天然知道
自己是哪个 cluster；新的设备级回调收到的是**所有** cluster，必须自己
`switch (path->cluster_id)`。

九个不同 cluster 的主属性 ID 都是 `0x0000`，所以**只 `switch (attribute_id)` 一定出错**。
清单见 [data-model.md](data-model.md#注意-attribute-id-会重名)。

如果你不想写一个大 switch（尤其是本来就有好几个 cluster 的固件），
用 `en2m_cluster_set_write_cb` / `_read_cb` 按 cluster 注册，形状和旧的 ops
几乎一样，还能带自己的 `ctx`：

```c
en2m_cluster_t *c = en2m_cluster_get(ep, EN2M_CLUSTER_FAN_CONTROL);
en2m_cluster_set_write_cb(c, on_fan_write, &s_fan);   /* ← 最接近旧 ops 的写法 */
```

### 第 5 步：`en2m_model_start` → `en2m_start`

```c
- en2m_config_t mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"};
- ESP_ERROR_CHECK(en2m_model_start(&mesh));
+ en2m_device_config_t cfg = {
+     .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"},
+     .attribute_write = on_write,
+ };
+ ESP_ERROR_CHECK(en2m_start(&cfg));
```

旧的 `en2m_config_t` 变成了新结构体里的 `.mesh` 成员，**字段一个没改**，
所以这一步是纯粹的包一层。

`en2m_model_start` 还留着（转调 `en2m_start`，五个回调全为 NULL），
但那样等于所有写入都直接提交、不落硬件，几乎肯定不是你想要的。

### 第 6 步：`en2m_model_notify` → `en2m_attribute_set`

```c
- gpio_set_level(PIN_RELAY, !on);
- en2m_model_notify(1, EN2M_CLUSTER_ON_OFF, true);
+ en2m_attribute_write(1, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!on));
```

注意这里选 `write` 而不是 `set`：

| 你原来在 notify 之前做了什么 | 换成 |
|---|---|
| 调了驱动 setter 改硬件 | `en2m_attribute_write`（让 `on_write` 去改硬件，**删掉**原来那行直接调驱动的代码） |
| 只是读了传感器 | `en2m_attribute_set` 或 `en2m_report_*` |

两条路径的区别是这次改动的核心，搞混会导致状态和硬件不一致。
见 [callbacks.md](architecture.md#45-两条属性访问路径故意不等价)。

`en2m_model_notify` 的第三个参数 `immediate` 没有对应物：
`en2m_attribute_set` 总是会排一次上报，节奏由 `min_report_interval_ms` 控制。
真的要"立刻发"，在 `en2m` 任务上调 `en2m_report_now()`。

### 第 7 步：改名的 API

| 旧 | 新 |
|---|---|
| `en2m_model_report()` | `en2m_report_now()` |
| `en2m_pairing()` | `en2m_get_pairing()` |
| `en2m_path_cost()` | `en2m_get_path_cost()` |
| `en2m_role()` | `en2m_get_role()` |
| `en2m_parent_mac(out)` | `en2m_get_parent_mac(out)` |
| `en2m_self_mac(out)` | `en2m_get_self_mac(out)` |
| `en2m_app_config_t` | `en2m_config_t` |
| `en2m_command_cb_t` | `en2m_frame_cb_t` |

后六个都有 `static inline` 兼容别名，能继续编。前两个也留着。
`en2m_cover_command_t` 和 15 个 `en2m_*_driver_t` **没有**兼容别名，
用到它们的代码编不过。

---

## 5. 新增的东西（旧版本没有，值得顺手用上）

迁移完之后这些都是免费拿到的：

| 新能力 | 怎么用 | 文档 |
|---|---|---|
| 属性断电不丢 | 执行器属性默认就开了 `persist`，什么都不用做 | [persistence.md](persistence.md) |
| 写失败不谎报状态 | `on_write` 返回非 `ESP_OK` | [callbacks.md](callbacks.md#1-attribute_write--把值落到硬件) |
| 12 个异步事件 | `esp_event_handler_register(EN2M_EVENT, …)` | [events.md](events.md) |
| ISR 安全的属性提交 | `en2m_attribute_set_from_isr` | [concurrency.md](concurrency.md) |
| 把活挪到组件任务 | `en2m_schedule` / `_from_isr` | [api-reference.md](api-reference.md#延迟执行) |
| 四种上报模式 + 限流 | `cfg.report_mode` / `min_report_interval_ms` | [reporting.md](reporting.md) |
| Identify 效果 | `cfg.identify` + `en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY)` | [callbacks.md](callbacks.md#5-identify--认人效果) |
| 下行 ACK / 重传 | 协调器侧自动，事件里能看到结果 | [state-flow.md](state-flow.md#7-下行-ack--重传状态机) |
| 上报超长自动降级 | 自动，不会截断 | [reporting.md](reporting.md#7-160-字节与降级策略) |
| Kconfig 调容量省 RAM | `sdkconfig.defaults` | [kconfig.md](kconfig.md) |

`Identify` cluster 和 `EN2M_ATTR_IDENTIFY_TIME` 属性也是新加的
（旧版本的 cluster 列表里没有 `EN2M_CLUSTER_IDENTIFY`）。

---

## 6. 协议兼容性

**好消息：空口协议没变。** 这次是纯粹的 API 重构。

| 东西 | 变了吗 |
|---|---|
| `EN2M_VERSION` | 没变，一直是 `2` |
| `en2m_pkt_t` 帧结构（221 字节） | **逐字节没变** |
| 六种 `EN2M_MSG_*` 类型 | 没变 |
| `EN2M_DATA_MAX`（160） | 没变 |
| 上报 JSON 的键名（`on`、`brightness`、`caps`、`node_role`…） | 没变 |
| 上报 JSON 的**生成方式** | 变了：从 cJSON 改成手写 `snprintf`，并加了三级降级 |
| 下行命令的解析 | 变了：多了 cluster 风格的 payload，旧的扁平键仍然支持 |

所以：

| 组合 | 结果 |
|---|---|
| 新设备 + 旧协调器 | **能通**。上报键名兼容，命令的扁平键仍然解析 |
| 旧设备 + 新协调器 | **能通** |
| 新 + 新 | 正常，而且上报超长时会降级而不是被截断 |

也就是说**可以一台一台升级**，不需要停机全刷。当然建议最终都升上来：
新版本的上报有降级保护，旧版本超过 160 字节会直接被截断成非法 JSON。

`components/en2m` 在 device 和 host 两个仓库里是**逐字节相同**的两份拷贝。
改了组件本身记得同步另一份。

上报载荷的完整格式见 [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md)，
cluster → HA 键的映射表见 [reporting.md](reporting.md#5-序列化cluster--ha-json-键)。
HA 集成（`espnow2mqtt-ha`）和 bridge 都不需要改。

---

## 7. 迁移完的自检清单

- [ ] 全工程 `grep -rn 'en2m_model_loop\|en2m_mesh_loop'`，结果为空
- [ ] 全工程 `grep -rn 'xTaskCreate'`，剩下的每一个都有非轮询的正当理由
- [ ] 全工程 `grep -rn '_driver_t'`，结果为空
- [ ] 全工程 `grep -rn 'en2m_model_notify\|en2m_model_start\|en2m_model_report'`，结果为空
- [ ] 每个 `attribute_write` / `attribute_read` 回调都**先** `switch (path->cluster_id)`
- [ ] 每个回调里不认识的路径返回 `ESP_ERR_NOT_SUPPORTED`（不是 `ESP_FAIL`）
- [ ] 所有硬件驱动的 `init` 都在 `en2m_start` **之前**
- [ ] 所有 `en2m_endpoint_create*` / `en2m_cluster_create` / `en2m_attribute_create`
      都在 `en2m_start` **之前**
- [ ] `cfg.user_ctx` 不指向 `app_main` 的局部变量
- [ ] 编译零警告（`idf.py fullclean && idf.py build`）
- [ ] 开机日志里没有 `no free … slot`、`is full`、`obsolete`
- [ ] 本地按键/输入走 `en2m_attribute_write`，而不是直接调驱动

对着 [usage.md 的反模式表](usage.md#4-反模式)再过一遍，那张表里列的
基本都是从旧写法惯性带过来的。

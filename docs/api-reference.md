# API 参考

全部公开函数、类型和宏。按头文件分组。
应用只需要 `#include "en2m.h"`，它把下面所有头都包进来。

"上下文"列的含义：**ISR** = 中断里可调，**任意** = 任意任务，
**建模期** = 必须在 `en2m_start` 之前。

- [en2m_model.h — 生命周期与数据模型构建](#en2m_modelh--生命周期与数据模型构建)
- [en2m_attr.h — 值、路径与属性访问](#en2m_attrh--值路径与属性访问)
- [en2m_event.h — 事件](#en2m_eventh--事件)
- [en2m_mesh.h — 传输](#en2m_meshh--传输)
- [en2m_proto.h — 帧与 MAC](#en2m_protoh--帧与-mac)
- [已弃用](#已弃用)
- [错误码速查](#错误码速查)

---

## en2m_model.h — 生命周期与数据模型构建

### `en2m_device_config_t`

```c
typedef struct {
    en2m_config_t mesh;                            /* 传输配置，见 en2m_mesh.h */

    en2m_attribute_write_cb_t   attribute_write;
    en2m_attribute_read_cb_t    attribute_read;
    en2m_attribute_changed_cb_t attribute_changed;
    en2m_command_handler_t      command;
    en2m_identify_cb_t          identify;
    void *user_ctx;                                /* 传给以上全部回调 */

    en2m_report_mode_t report_mode;                /* 默认 EN2M_REPORT_DEFAULT */
    uint32_t report_interval_ms;                   /* 0 = LEAF 30000，其他 15000 */
    uint32_t min_report_interval_ms;               /* 0 = 1000 */
    uint32_t task_stack_size;                      /* 0 = 4096 */
    uint8_t  task_priority;                        /* 0 = 5 */
} en2m_device_config_t;
```

`en2m_start` 会**拷贝**整个结构体，所以可以传栈上的变量。
但 `user_ctx` 指向的东西、以及 `mesh.name` / `mesh.model` / `mesh.fw`
指向的字符串必须在整个运行期有效（`name` 和 `model` 会被拷进内部缓冲，
`fw` 只是存指针）。

### `en2m_report_mode_t`

| 值 | 变化触发 | 周期触发 |
|---|:-:|:-:|
| `EN2M_REPORT_DEFAULT` | ✓ | ✓ |
| `EN2M_REPORT_PERIODIC_ONLY` | | ✓ |
| `EN2M_REPORT_ON_CHANGE_ONLY` | ✓ | |
| `EN2M_REPORT_MANUAL` | | |

### `en2m_device_type_t`

16 个值，配方见 [data-model.md](data-model.md#5-设备类型配方)。

### 构建

```c
en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id);
```
建一个 endpoint。**上下文：建模期。**

| | |
|---|---|
| `endpoint_id` | 1–254。0 或 255 返回 `NULL` |
| 返回 | 句柄；重复建同一 id 或槽位用尽时 `NULL`（都会打 error） |

---

```c
en2m_endpoint_t *en2m_endpoint_get(uint8_t endpoint_id);
```
查已存在的 endpoint。**上下文：任意。** 不存在返回 `NULL`。

---

```c
en2m_endpoint_t *en2m_endpoint_create_device(uint8_t endpoint_id, en2m_device_type_t type);
```
建 endpoint 并按配方填 cluster。**上下文：建模期。**
等价于 `en2m_endpoint_create` + `en2m_endpoint_add_device_type`。
任一步失败返回 `NULL`。

---

```c
esp_err_t en2m_endpoint_add_device_type(en2m_endpoint_t *endpoint, en2m_device_type_t type);
```
把一个设备类型的 cluster 加到已有 endpoint 上。**上下文：建模期。**
可以反复调用叠加多个类型。

| 返回 | |
|---|---|
| `ESP_OK` | |
| `ESP_ERR_INVALID_ARG` | `endpoint` 为 `NULL`，或 `type` 不认识 |

---

```c
en2m_cluster_t *en2m_cluster_create(en2m_endpoint_t *endpoint, uint16_t cluster_id);
```
建 cluster，**自动填好该 cluster 的默认属性**。**上下文：建模期。**

**注意返回值语义**：cluster 已存在时返回**已有的那个**（不是 `NULL`），
方便重复调用。只有 `endpoint == NULL` 或槽位用尽时返回 `NULL`。

---

```c
en2m_cluster_t *en2m_cluster_get(en2m_endpoint_t *endpoint, uint16_t cluster_id);
```
查 cluster。**上下文：任意。** 不存在返回 `NULL`。

---

```c
esp_err_t en2m_attribute_create(en2m_cluster_t *cluster, uint16_t attribute_id,
                                en2m_value_t default_value, bool persist);
```
手动加属性。**上下文：建模期。**

| | |
|---|---|
| `default_value` | 同时**定死属性的类型** |
| `persist` | 进 NVS。执行器状态用 `true`，传感器读数用 `false` |
| 返回 `ESP_OK` | 成功，**或属性已存在**（此时不改动） |
| 返回 `ESP_ERR_INVALID_ARG` | `cluster` 为 `NULL` |
| 返回 `ESP_ERR_NO_MEM` | cluster 的属性槽用尽 |

---

```c
esp_err_t en2m_cluster_set_write_cb  (en2m_cluster_t *c, en2m_attribute_write_cb_t cb, void *ctx);
esp_err_t en2m_cluster_set_read_cb   (en2m_cluster_t *c, en2m_attribute_read_cb_t  cb, void *ctx);
esp_err_t en2m_cluster_set_command_cb(en2m_cluster_t *c, en2m_command_handler_t    cb, void *ctx);
```
按 cluster 注册回调，**优先于设备级回调**。**上下文：任意**（有锁保护，
运行期改也安全）。`cb` 传 `NULL` 相当于取消。
`ESP_ERR_INVALID_ARG` 表示 `c` 为 `NULL`。

### 生命周期

```c
esp_err_t en2m_start(const en2m_device_config_t *config);
```
启动数据模型和传输。**上下文：任意任务（不要在 en2m 任务里调）。**

内部顺序见 [state-flow.md](state-flow.md#1-启动时序)。

| 返回 | |
|---|---|
| `ESP_OK` | |
| `ESP_ERR_INVALID_ARG` | `config` 为 `NULL` |
| `ESP_ERR_INVALID_STATE` | 已经启动过 |
| 其他 | `en2m_mesh_init` 的失败原因（Wi-Fi / ESP-NOW / NVS / 任务创建） |

---

```c
esp_err_t en2m_stop(void);
```
停任务、拆传输。**数据模型和属性值保留**，可以再 `en2m_start`。
**上下文：任意任务（不要在 en2m 任务里调，它会等该任务退出）。**
最多等约 1 秒。未启动时返回 `ESP_ERR_INVALID_STATE`。

---

```c
bool en2m_is_started(void);
```
**上下文：任意 + en2m 任务。**

### 上报

```c
esp_err_t en2m_report_now(void);
```
立刻上报。**上下文：任意 + en2m 任务。**

**行为取决于调用上下文**：

| 从哪调 | 行为 |
|---|---|
| en2m 任务（回调里） | 立即 `dm_refresh()` + 发送，**不受 `min_report_interval_ms` 约束** |
| 其他任务 | 降级成 `en2m_report_schedule(0)`，**受约束** |

未启动时返回 `ESP_ERR_INVALID_STATE`。

---

```c
esp_err_t en2m_report_schedule(uint32_t delay_ms);
```
安排一次上报，和已有的安排合并（**最早的赢**）。
**上下文：任意 + en2m 任务。**
实际时刻不早于 `last_report_ms + min_report_interval_ms`。
未启动时返回 `ESP_ERR_INVALID_STATE`。

### 延迟执行

```c
typedef void (*en2m_work_fn_t)(void *arg);

esp_err_t en2m_schedule(en2m_work_fn_t fn, void *arg);
esp_err_t en2m_schedule_from_isr(en2m_work_fn_t fn, void *arg, BaseType_t *higher_prio_task_woken);
```
把 `fn(arg)` 排到 en2m 任务上执行。**上下文：`_from_isr` 版本可在 ISR 调，
另一个不行。**

| 返回 | |
|---|---|
| `ESP_OK` | 已入队 |
| `ESP_ERR_INVALID_ARG` | `fn` 为 `NULL` |
| `ESP_ERR_INVALID_STATE` | 队列还不存在（`en2m_mesh_init` 之前） |
| `ESP_ERR_NO_MEM` | **队列满** |

ISR 版本要按 FreeRTOS 惯例处理 `woken`：

```c
BaseType_t woken = pdFALSE;
en2m_schedule_from_isr(fn, arg, &woken);
if (woken) {
    portYIELD_FROM_ISR();
}
```

### 容量宏

```c
#define EN2M_MAX_ENDPOINTS               4    /* CONFIG_EN2M_MAX_ENDPOINTS */
#define EN2M_MAX_CLUSTERS_PER_ENDPOINT   8    /* CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT */
#define EN2M_MAX_ATTRIBUTES_PER_CLUSTER  6    /* CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER */
```

---

## en2m_attr.h — 值、路径与属性访问

### 标识符

```c
typedef enum { ... } en2m_cluster_id_t;     /* 16 个，Matter 编号 */
typedef enum { ... } en2m_attribute_id_t;   /* 按 cluster 分域 */
typedef enum { ... } en2m_command_id_t;     /* 19 个 */
typedef enum { ... } en2m_thermostat_mode_t;
typedef enum { ... } en2m_fan_mode_t;
typedef enum { ... } en2m_lock_state_t;
```

全表见 [data-model.md](data-model.md#3-cluster-与-attribute-全表)。

### 值

```c
typedef enum {
    EN2M_VAL_NULL = 0, EN2M_VAL_BOOL, EN2M_VAL_U8, EN2M_VAL_U16, EN2M_VAL_U32,
    EN2M_VAL_I16, EN2M_VAL_I32, EN2M_VAL_I64, EN2M_VAL_ENUM8,
} en2m_val_type_t;

typedef struct {
    en2m_val_type_t type;
    union { bool b; uint8_t u8; uint16_t u16; uint32_t u32;
            int16_t i16; int32_t i32; int64_t i64; uint8_t e8; } v;
} en2m_value_t;

typedef struct {
    uint8_t endpoint_id; uint16_t cluster_id; uint16_t attribute_id;
} en2m_attr_path_t;
```

构造器，全是 `static inline`，**ISR 可用**：

```c
en2m_value_t en2m_bool(bool), en2m_u8(uint8_t), en2m_u16(uint16_t), en2m_u32(uint32_t),
             en2m_i16(int16_t), en2m_i32(int32_t), en2m_i64(int64_t), en2m_enum8(uint8_t);
```

```c
int64_t en2m_value_as_int(const en2m_value_t *value);
```
任何值的整数视图。`bool` → 0/1，`NULL` 或 `value == NULL` → 0。**ISR 可用。**

```c
bool en2m_value_equal(const en2m_value_t *a, const en2m_value_t *b);
```
**类型和数值都相同**才为 `true`。两个都为 `NULL` 时返回 `true`
（`a == b`），一个为 `NULL` 时 `false`。**ISR 可用。**

### 回调类型

```c
typedef esp_err_t (*en2m_attribute_write_cb_t)(const en2m_attr_path_t *path,
                                               const en2m_value_t *value, void *ctx);
typedef esp_err_t (*en2m_attribute_read_cb_t)(const en2m_attr_path_t *path,
                                              en2m_value_t *out_value, void *ctx);
typedef void      (*en2m_attribute_changed_cb_t)(const en2m_attr_path_t *path,
                                                 const en2m_value_t *value, void *ctx);
typedef esp_err_t (*en2m_command_handler_t)(const en2m_command_t *command, void *ctx);
typedef void      (*en2m_identify_cb_t)(uint8_t endpoint_id, uint16_t seconds, void *ctx);
```

契约见 [callbacks.md](callbacks.md)。

### 命令

```c
typedef struct {
    uint8_t  endpoint_id;
    uint16_t cluster_id;
    en2m_command_id_t id;
    const char *name;         /* 收到的原始命令名，永不为 NULL */
    en2m_value_t arg;         /* 主参数，没有时 type == EN2M_VAL_NULL */
    uint16_t transaction_id;
    const char *json;         /* 原始 payload */
} en2m_command_t;
```

### 属性访问

```c
esp_err_t en2m_attribute_get(uint8_t ep, uint16_t cluster, uint16_t attr, en2m_value_t *out);
```
读缓存值。**上下文：任意 + en2m 任务。**

| 返回 | |
|---|---|
| `ESP_OK` | `*out` 已填 |
| `ESP_ERR_INVALID_ARG` | `out` 为 `NULL` |
| `ESP_ERR_NOT_FOUND` | 没有这个属性（`*out` 不变） |

---

```c
esp_err_t en2m_attribute_set(uint8_t ep, uint16_t cluster, uint16_t attr, en2m_value_t value);
```
**传感器路径**：提交 + 通知 + 安排上报。**不调 write 回调。**
**上下文：任意 + en2m 任务。**

值会被强制成属性声明的类型。和旧值相等时什么都不做（仍返回 `ESP_OK`）。

| 返回 | |
|---|---|
| `ESP_OK` | 提交了，或值没变 |
| `ESP_ERR_NOT_FOUND` | 没有这个属性（会打 WARN） |

`attribute_changed` 回调在**调用者任务**上同步执行。

---

```c
esp_err_t en2m_attribute_set_from_isr(uint8_t ep, uint16_t cluster, uint16_t attr,
                                      en2m_value_t value, BaseType_t *higher_prio_task_woken);
```
ISR 安全版本。**上下文：ISR + 任意。** 真正的提交推迟到 en2m 任务。

| 返回 | |
|---|---|
| `ESP_OK` | 已入队（**不代表属性存在**，路径错了要到任务里才发现并打 WARN） |
| `ESP_ERR_INVALID_STATE` | 队列还不存在 |
| `ESP_ERR_NO_MEM` | 队列满 |

---

```c
esp_err_t en2m_attribute_write(uint8_t ep, uint16_t cluster, uint16_t attr, en2m_value_t value);
```
**执行器路径**：先调 write 回调，成功才提交。**上下文：任意 + en2m 任务。**

三级 fall-through：cluster 回调 → 设备回调 → 裸提交。

| 返回 | |
|---|---|
| `ESP_OK` | 硬件接受了（或没人管），已提交 |
| `ESP_ERR_NOT_FOUND` | 没有这个属性 |
| 回调返回的 err | 硬件拒绝，**未提交**，打 WARN |

write 回调在**调用者任务**上执行。

### 便利上报函数

全部是 `en2m_attribute_set` 的包装，**上下文：任意 + en2m 任务**，
不能在 ISR 调。返回值同 `en2m_attribute_set`。

| 函数 | 目标 | 单位 / 范围 |
|---|---|---|
| `en2m_report_on_off(ep, bool on)` | OnOff | |
| `en2m_report_level(ep, uint8_t level)` | LevelControl | 0–254 |
| `en2m_report_color_temperature(ep, uint16_t mireds)` | ColorControl | mired |
| `en2m_report_temperature(ep, int16_t centi_celsius)` | TemperatureMeasurement | **0.01 °C** |
| `en2m_report_humidity(ep, uint16_t centi_percent)` | RelativeHumidity | **0.01 %** |
| `en2m_report_pressure(ep, int32_t deci_hpa)` | PressureMeasurement | **0.1 hPa** |
| `en2m_report_illuminance(ep, uint32_t lux)` | Illuminance | lux |
| `en2m_report_boolean_state(ep, bool state)` | BooleanState | |
| `en2m_report_occupancy(ep, bool occupied)` | Occupancy | |
| `en2m_report_smoke(ep, bool alarm)` | SmokeCO / SmokeState | |
| `en2m_report_co(ep, bool alarm)` | SmokeCO / COState | |
| `en2m_report_lock_state(ep, en2m_lock_state_t s)` | DoorLock | |
| `en2m_report_cover_position(ep, uint8_t closed_pct)` | WindowCovering | 0–100，**内部 clamp** |
| `en2m_report_fan(ep, en2m_fan_mode_t mode, uint8_t pct)` | FanControl | 写两个属性，`pct` **内部 clamp** |
| `en2m_report_power(ep, int32_t milliwatts)` | ElectricalPower | **毫瓦** |
| `en2m_report_energy(ep, int64_t milliwatt_hours)` | ElectricalPower | **毫瓦时** |

只有 `en2m_report_cover_position` 和 `en2m_report_fan` 会帮你 clamp，
其他函数的范围要自己保证（见
[data-model.md](data-model.md#类型强制重要)）。

`en2m_report_fan` 先写 `FAN_MODE`，成功后再写 `PERCENT_SETTING`，
返回第一个失败的错误码。

---

## en2m_event.h — 事件

```c
ESP_EVENT_DECLARE_BASE(EN2M_EVENT);
#define EN2M_EVENT_ANY ESP_EVENT_ANY_ID

typedef enum {
    EN2M_EVENT_STARTED, EN2M_EVENT_STOPPED,
    EN2M_EVENT_PARENT_FOUND, EN2M_EVENT_PARENT_LOST,
    EN2M_EVENT_PAIRING_CHANGED,
    EN2M_EVENT_ATTRIBUTE_UPDATED, EN2M_EVENT_COMMAND_RECEIVED,
    EN2M_EVENT_REPORT_SENT, EN2M_EVENT_IDENTIFY,
    EN2M_EVENT_ACK_RECEIVED, EN2M_EVENT_ACK_TIMEOUT,
    EN2M_EVENT_RX_DROPPED,
} en2m_event_id_t;
```

```c
esp_err_t en2m_event_handler_register(int32_t event_id, esp_event_handler_t handler, void *handler_arg);
esp_err_t en2m_event_handler_unregister(int32_t event_id, esp_event_handler_t handler);
```
**上下文：任意 + en2m 任务。** 可以在 `en2m_start` 之前调
（会自动建默认事件循环），所以不会漏掉 `EN2M_EVENT_STARTED`。

payload 结构见 [events.md](events.md#4-payload-结构)。

---

## en2m_mesh.h — 传输

### `en2m_config_t`

```c
typedef struct {
    en2m_role_t role;             /* 必填 */
    const char *model;            /* NULL → EN2M_DEVICE_MODEL，拷进 char[12] */
    const char *name;             /* NULL → EN2M_DEVICE_NAME，拷进 char[16] */
    const char *fw;               /* 只存指针 */
    uint8_t channel;              /* 0 → EN2M_WIFI_CHANNEL */
    en2m_uplink_cb_t on_uplink;   /* 协调器专用 */
    en2m_frame_cb_t  on_command;  /* 裸帧接管；NULL 时交互层装自己的解码器 */
    en2m_log_cb_t    on_log;      /* NULL → ESP_LOG */
    void *user_ctx;
    uint8_t  max_retries;         /* 0 → EN2M_CMD_RETRIES (3) */
    uint32_t retry_interval_ms;   /* 0 → EN2M_CMD_RETRY_MS (400) */
} en2m_config_t;
```

```c
typedef void (*en2m_uplink_cb_t)(const en2m_pkt_t *pkt, int8_t rssi,
                                 const uint8_t from_mac[6], void *user_ctx);
typedef void (*en2m_frame_cb_t)(const en2m_pkt_t *pkt, void *user_ctx);
typedef void (*en2m_log_cb_t)(const char *msg, void *user_ctx);
```

### 起停

```c
esp_err_t en2m_mesh_init(const en2m_config_t *config);
void      en2m_mesh_deinit(void);
```
`en2m_start` 内部会调 `en2m_mesh_init`。**没有 cluster 的固件**
（协调器、纯 router）直接调它。**上下文：任意任务。**

| 返回 | |
|---|---|
| `ESP_OK` | |
| `ESP_ERR_INVALID_ARG` | `config` 为 `NULL`，或 `role` 不合法 |
| `ESP_ERR_INVALID_STATE` | 已初始化 |
| `ESP_ERR_NO_MEM` | 锁/队列/任务创建失败 |
| 其他 | Wi-Fi、ESP-NOW、NVS 的失败原因 |

`esp_netif_init` 和 `esp_event_loop_create_default` 的
`ESP_ERR_INVALID_STATE` 被视为成功，所以应用可以先自己建。

### 发送

```c
esp_err_t en2m_send_uplink(uint8_t msg_type, uint16_t cmd_id, const uint8_t *data, uint8_t len);
```
上行。**上下文：任意 + en2m 任务。**

| 返回 | |
|---|---|
| `ESP_OK` | 交给 ESP-NOW 了 |
| `ESP_ERR_NOT_SUPPORTED` | 角色是 COORDINATOR |
| `ESP_ERR_INVALID_STATE` | 未初始化；或**无父节点且 `msg_type` 不是 HELLO/HEARTBEAT** |
| `ESP_FAIL` | 加 peer 失败 |

`len > EN2M_DATA_MAX` 时**静默截断**到 160。

---

```c
esp_err_t en2m_send_downlink(const uint8_t dest_mac[6], uint16_t cmd_id,
                             const uint8_t *data, uint8_t len);
```
下行。**上下文：任意 + en2m 任务。**
`cmd_id != 0` 时自动进入重传跟踪，结果通过
`EN2M_EVENT_ACK_RECEIVED` / `_TIMEOUT` 报出。

| 返回 | |
|---|---|
| `ESP_OK` | |
| `ESP_ERR_INVALID_ARG` | `dest_mac` 为 `NULL` |
| `ESP_ERR_NOT_SUPPORTED` | 角色不是 COORDINATOR |
| `ESP_ERR_INVALID_STATE` | 未初始化 |

---

```c
uint16_t en2m_next_cmd_id(void);
```
分配一个事务 id，**永不为 0**。**上下文：任意 + en2m 任务。**

### 状态

| 函数 | 说明 |
|---|---|
| `void en2m_set_pairing(bool enabled)` | 开/关配网窗口。真的改变时发 `PAIRING_CHANGED` |
| `bool en2m_get_pairing(void)` | |
| `bool en2m_has_parent(void)` | 协调器恒为 `true` |
| `uint8_t en2m_get_path_cost(void)` | **255 = 无父**，协调器为 0 |
| `en2m_role_t en2m_get_role(void)` | |
| `void en2m_get_parent_mac(uint8_t out[6])` | 无父时填**广播地址** |
| `void en2m_get_self_mac(uint8_t out[6])` | |
| `void en2m_set_name(const char *name)` | 运行期改 MQTT slug，`NULL` 时忽略 |

### 路由

```c
bool en2m_lookup_route(const uint8_t dest[6], uint8_t next_hop[6]);
void en2m_forget_route(const uint8_t dest[6]);
void en2m_clear_routes(void);
```
**这三个没有内部加锁**，只适合在 en2m 上下文（`on_uplink` 回调里）调用。

### 宏

全部可通过 Kconfig 覆盖，见 [kconfig.md](kconfig.md)。

| 宏 | 默认 |
|---|---|
| `EN2M_WIFI_CHANNEL` | 1 |
| `EN2M_HOP_LIMIT` | 8 |
| `EN2M_MAX_ROUTES` | 32 |
| `EN2M_MAX_NEIGHBORS` | 16 |
| `EN2M_BEACON_MS_DEFAULT` | 5000 |
| `EN2M_PARENT_STALE_MS` | 20000 |
| `EN2M_ROUTE_STALE_MS` | 120000 |
| `EN2M_OFFLINE_MS` | 90000 |
| `EN2M_HEARTBEAT_MS` | 30000 |
| `EN2M_QUEUE_LEN` | 8 |
| `EN2M_MAX_PENDING` | 4 |
| `EN2M_CMD_RETRIES` | 3 |
| `EN2M_CMD_RETRY_MS` | 400 |
| `EN2M_DEVICE_MODEL` | `"c3-node"` |
| `EN2M_DEVICE_NAME` | `"node1"` |
| `EN2M_FW_VERSION` | `"0.4.0-idf"` |

---

## en2m_proto.h — 帧与 MAC

```c
#define EN2M_MAGIC    0xA5
#define EN2M_VERSION  2
#define EN2M_DATA_MAX 160

typedef enum { EN2M_ROLE_COORDINATOR = 1, EN2M_ROLE_ROUTER = 2, EN2M_ROLE_LEAF = 3 } en2m_role_t;

typedef enum {
    EN2M_MSG_BEACON = 1, EN2M_MSG_HELLO = 2, EN2M_MSG_STATE = 3,
    EN2M_MSG_CMD = 4, EN2M_MSG_ACK = 5, EN2M_MSG_HEARTBEAT = 6,
} en2m_msg_type_t;

typedef enum { EN2M_FLAG_PAIRING = 0x01 } en2m_flags_t;

typedef struct __attribute__((packed)) { ... } en2m_pkt_t;   /* 见 mesh.md */
```

MAC 辅助，全部 **ISR 可用**：

```c
static inline void en2m_mac_broadcast(uint8_t mac[6]);
static inline bool en2m_mac_is_broadcast(const uint8_t mac[6]);
static inline bool en2m_mac_equal(const uint8_t a[6], const uint8_t b[6]);
static inline void en2m_mac_copy(uint8_t dst[6], const uint8_t src[6]);

void en2m_mac_to_str(const uint8_t mac[6], char buf[18]);   /* "AA:BB:CC:DD:EE:FF" */
bool en2m_mac_from_str(const char *str, uint8_t out[6]);    /* 成功返回 true */
```

`en2m_mac_to_str` 的缓冲必须是 **18 字节**（17 字符 + `'\0'`）。

---

## 已弃用

留着只为了让旧固件还能编过。**新代码不要用。**

| 弃用的 | 用这个 |
|---|---|
| `en2m_model_start(&mesh_cfg)` | `en2m_start(&device_cfg)` |
| `en2m_model_loop()` | 删掉。空函数，第一次调用打一条 WARN |
| `en2m_mesh_loop()` | 删掉。同上 |
| `en2m_model_report()` | `en2m_report_now()` |
| `en2m_model_notify(ep, cluster, immediate)` | `en2m_attribute_set()`（它自己会上报） |
| `en2m_command_cb_t` | `en2m_frame_cb_t` |
| `en2m_app_config_t` | `en2m_config_t` |
| `en2m_pairing()` | `en2m_get_pairing()` |
| `en2m_path_cost()` | `en2m_get_path_cost()` |
| `en2m_role()` | `en2m_get_role()` |
| `en2m_parent_mac(out)` | `en2m_get_parent_mac(out)` |
| `en2m_self_mac(out)` | `en2m_get_self_mac(out)` |

从 driver-ops 版本升级见 [migration.md](migration.md)。

---

## 错误码速查

| 错误码 | 在 en2m 里的含义 |
|---|---|
| `ESP_OK` | 成功 |
| `ESP_ERR_INVALID_ARG` | 参数为 `NULL`，或 id 不合法 |
| `ESP_ERR_INVALID_STATE` | **顺序错了**：未启动 / 已启动 / 未初始化 / **无父节点** |
| `ESP_ERR_NOT_FOUND` | 属性或 endpoint 不存在 |
| `ESP_ERR_NOT_SUPPORTED` | 在回调返回值里 = "**我不管，交给下一级**"；在 API 返回值里 = 角色不对 |
| `ESP_ERR_NO_MEM` | 槽位或队列用尽 |
| `ESP_FAIL` | ESP-NOW 层面失败（通常是加 peer 失败） |

**`ESP_ERR_NOT_SUPPORTED` 在回调返回值里有特殊含义**，不是错误。
详见 [callbacks.md](callbacks.md#6-三级-fall-through)。

---

## 相关文档

- 回调的完整契约 → [callbacks.md](callbacks.md)
- 每个 API 的调用上下文汇总表 → [concurrency.md](concurrency.md#6-每个公开-api-的可调用上下文)
- 从零写一个设备 → [usage.md](usage.md)
- 英文速查 → [../components/en2m/README.md](../components/en2m/README.md)

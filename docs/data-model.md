# 数据模型

`en2m` 的数据模型和 Matter 是同一套概念：**endpoint → cluster → attribute** 三级。
ID 在有对应概念的地方直接用 Matter 的编号，这样这套模型既能映射到 Matter，
也能映射到 Home Assistant 的 MQTT 平台。

- [1. 三级模型](#1-三级模型)
- [2. 值类型系统](#2-值类型系统)
- [3. Cluster 与 Attribute 全表](#3-cluster-与-attribute-全表)
- [4. 枚举值](#4-枚举值)
- [5. 设备类型配方](#5-设备类型配方)
- [6. 构建 API](#6-构建-api)
- [7. 容量与内存](#7-容量与内存)

---

## 1. 三级模型

```
设备（一块 C3，一个 MAC）
└── endpoint 1..254          一个物理功能单元
    ├── cluster 0x0006 (OnOff)          一组相关能力
    │   └── attribute 0x0000 (OnOff)    一个具体的值
    ├── cluster 0x0008 (LevelControl)
    │   └── attribute 0x0000 (CurrentLevel)
    └── cluster 0x0300 (ColorControl)
        └── attribute 0x0007 (ColorTemperatureMireds)
```

一个属性由三元组唯一确定，库里叫 `en2m_attr_path_t`：

```c
typedef struct {
    uint8_t  endpoint_id;
    uint16_t cluster_id;
    uint16_t attribute_id;
} en2m_attr_path_t;
```

### endpoint 编号规则

- 合法范围 **1–254**。`0` 和 `255` 会被 `en2m_endpoint_create` 拒绝并打 error。
- 单功能设备一律用 `1`。
- 多功能设备（比如一个双路继电器）用 `1` 和 `2`。

### 什么时候需要多个 endpoint

坦率地说：**大多数情况不需要，而且目前多 endpoint 有一个明确的限制**。

上报出去的 JSON 用的是**扁平的全局键**（`switch`、`temperature`、`brightness` …），
键里不带 endpoint 编号。序列化时同一个 cluster 只会被处理一次
（`en2m_report_build` 用 `seen_ids[]` 去重），**编号最小的 endpoint 赢**。

所以：

| 场景 | 可行？ |
|---|---|
| endpoint 1 = OnOff，endpoint 2 = TemperatureMeasurement | ✓ cluster 不同，键不冲突 |
| endpoint 1 = OnOff，endpoint 2 = OnOff（双路继电器） | ✗ 两路都映射到同一个 `switch` 键，第二路上报不出去 |

要做双路开关，当前的正确做法是**两块 C3**，或者等一个带 endpoint 前缀的协议版本。
命令下行方向是支持 `ep` 寻址的（`{"ep":2,"cluster":"on_off","command":"on"}`），
只有上报方向有这个限制。

---

## 2. 值类型系统

属性值是一个**带 tag 的联合体**，不是裸整数：

```c
typedef enum {
    EN2M_VAL_NULL = 0,
    EN2M_VAL_BOOL,
    EN2M_VAL_U8,
    EN2M_VAL_U16,
    EN2M_VAL_U32,
    EN2M_VAL_I16,
    EN2M_VAL_I32,
    EN2M_VAL_I64,
    EN2M_VAL_ENUM8,
} en2m_val_type_t;

typedef struct {
    en2m_val_type_t type;
    union {
        bool     b;
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        int16_t  i16;
        int32_t  i32;
        int64_t  i64;
        uint8_t  e8;
    } v;
} en2m_value_t;
```

### 构造器

全是 `static inline`，没有副作用，**中断里也能用**：

```c
en2m_value_t en2m_bool (bool     value);
en2m_value_t en2m_u8   (uint8_t  value);
en2m_value_t en2m_u16  (uint16_t value);
en2m_value_t en2m_u32  (uint32_t value);
en2m_value_t en2m_i16  (int16_t  value);
en2m_value_t en2m_i32  (int32_t  value);
en2m_value_t en2m_i64  (int64_t  value);
en2m_value_t en2m_enum8(uint8_t  value);
```

### 工具函数

```c
/* 任何值的整数视图。bool → 0/1，NULL → 0。 */
int64_t en2m_value_as_int(const en2m_value_t *value);

/* 类型和数值都相同才为 true。 */
bool en2m_value_equal(const en2m_value_t *a, const en2m_value_t *b);
```

读值时要么直接访问对应的联合体成员（你知道类型的时候），要么用
`en2m_value_as_int()`（写通用代码的时候）：

```c
en2m_value_t v;
en2m_attribute_get(1, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, &v);
bool on = v.v.b;                        /* 知道是 bool */

int64_t raw = en2m_value_as_int(&v);    /* 通用写法，得到 0/1 */
```

### 类型强制（重要）

属性在**创建时**就定死了类型。之后写进来的值会被 `en2m_value_coerce()`
重新塑形成那个类型：

```c
/* CURRENT_LEVEL 声明为 EN2M_VAL_U8 */
en2m_attribute_set(1, EN2M_CLUSTER_LEVEL_CONTROL, EN2M_ATTR_CURRENT_LEVEL,
                   en2m_i32(200));
/* 实际存的是 en2m_u8(200) */
```

**好处**：驱动可以传自己最自然的整数宽度，不必记住属性声明成了什么。
一个返回 `int` 的 ADC 读数直接 `en2m_i32()` 包一下就行。

**代价**：溢出被静默截断。`en2m_i32(300)` 写进 u8 属性会变成 `44`
（300 & 0xFF）。**范围要自己 clamp**。库只在少数内建路径上帮你 clamp
（`MOVE_TO_LEVEL` 限 0–254、位置和百分比限 0–100、`en2m_report_cover_position`
和 `en2m_report_fan` 限 100），其他地方不管。

`EN2M_VAL_NULL` 类型的属性是个例外：强制会原样返回输入值，
也就是这个属性第一次被写时会"定型"成写进来的类型。一般不要这么用。

---

## 3. Cluster 与 Attribute 全表

`en2m_cluster_create()` 会**自动填好**该 cluster 应有的属性和默认值，
所以正常情况下你不需要手动 `en2m_attribute_create`。

下表的"持久"列表示该属性默认是否进 NVS，见 [persistence.md](persistence.md)。

### 控制类

| Cluster | ID | Attribute | ID | 类型 | 默认值 | 持久 | 单位 / 范围 |
|---|---|---|---|---|---|:-:|---|
| Identify | `0x0003` | `IDENTIFY_TIME` | `0x0000` | u16 | 0 | | 秒，每秒自减 |
| OnOff | `0x0006` | `ON_OFF` | `0x0000` | bool | false | ✓ | |
| LevelControl | `0x0008` | `CURRENT_LEVEL` | `0x0000` | u8 | 254 | ✓ | 0–254 |
| ColorControl | `0x0300` | `COLOR_TEMPERATURE_MIREDS` | `0x0007` | u16 | 300 | ✓ | mired |
| DoorLock | `0x0101` | `LOCK_STATE` | `0x0000` | enum8 | `LOCKED` | ✓ | 见枚举表 |
| WindowCovering | `0x0102` | `CURRENT_POSITION_LIFT_PERCENT` | `0x0008` | u8 | 0 | ✓ | 0 = 全开，100 = 全闭 |
| FanControl | `0x0202` | `FAN_MODE` | `0x0000` | enum8 | `OFF` | ✓ | 见枚举表 |
| | | `PERCENT_SETTING` | `0x0002` | u8 | 0 | ✓ | 0–100 |
| Thermostat | `0x0201` | `LOCAL_TEMPERATURE` | `0x0000` | i16 | 0 | | 0.01 °C |
| | | `OCCUPIED_COOLING_SETPOINT` | `0x0011` | i16 | 2400 | ✓ | 0.01 °C（= 24.00 °C） |
| | | `OCCUPIED_HEATING_SETPOINT` | `0x0012` | i16 | 2100 | ✓ | 0.01 °C（= 21.00 °C） |
| | | `SYSTEM_MODE` | `0x001C` | enum8 | `OFF` | ✓ | 见枚举表 |

### 传感类

| Cluster | ID | Attribute | ID | 类型 | 默认值 | 持久 | 单位 |
|---|---|---|---|---|---|:-:|---|
| BooleanState | `0x0045` | `STATE_VALUE` | `0x0000` | bool | false | | 门磁：true = 触发 |
| Occupancy | `0x0406` | `OCCUPANCY` | `0x0000` | bool | false | | true = 有人 |
| Illuminance | `0x0400` | `MEASURED_VALUE` | `0x0000` | u32 | 0 | | lux |
| TemperatureMeasurement | `0x0402` | `MEASURED_VALUE` | `0x0000` | i16 | 0 | | 0.01 °C |
| RelativeHumidity | `0x0405` | `MEASURED_VALUE` | `0x0000` | u16 | 0 | | 0.01 %RH |
| PressureMeasurement | `0x0403` | `MEASURED_VALUE` | `0x0000` | i32 | 0 | | 0.1 hPa |
| SmokeCO | `0x005C` | `SMOKE_STATE` | `0x0001` | bool | false | | true = 报警 |
| | | `CO_STATE` | `0x0002` | bool | false | | true = 报警 |
| ElectricalPower | `0x0B04` | `ACTIVE_POWER_MW` | `0x000A` | i32 | 0 | | 毫瓦 |
| | | `ENERGY_MWH` | `0x0011` | i64 | 0 | ✓ | 毫瓦时（累计，所以持久化） |

### 注意 attribute ID 会重名

好几个 cluster 的主属性 ID 都是 `0x0000`，头文件里给了不同的名字方便阅读，
但它们**值相同**：

```c
EN2M_ATTR_IDENTIFY_TIME  == 0x0000
EN2M_ATTR_ON_OFF         == 0x0000
EN2M_ATTR_CURRENT_LEVEL  == 0x0000
EN2M_ATTR_STATE_VALUE    == 0x0000
EN2M_ATTR_OCCUPANCY      == 0x0000
EN2M_ATTR_MEASURED_VALUE == 0x0000
EN2M_ATTR_LOCK_STATE     == 0x0000
EN2M_ATTR_LOCAL_TEMPERATURE == 0x0000
EN2M_ATTR_FAN_MODE       == 0x0000
```

这不是 bug——attribute ID 的作用域**是 cluster**。所以回调里分发必须
**先看 `cluster_id`，再看 `attribute_id`**：

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    switch (path->cluster_id) {            /* ← 先分 cluster */
    case EN2M_CLUSTER_ON_OFF:
        return relay_set(value->v.b);
    case EN2M_CLUSTER_LEVEL_CONTROL:
        return pwm_set(value->v.u8);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
```

只看 `attribute_id == 0x0000` 就会把 OnOff、亮度、温度混在一起。

### 单位约定

| 物理量 | 内部表示 | 上报给 HA |
|---|---|---|
| 温度 | i16，0.01 °C | 除以 100 → °C |
| 湿度 | u16，0.01 %RH | 除以 100 → % |
| 气压 | i32，0.1 hPa | 除以 10 → hPa |
| 功率 | i32，毫瓦 | 除以 1000 → W |
| 电量 | i64，毫瓦时 | 除以 1000 → Wh |
| 照度 | u32，lux | 原样 |
| 亮度 | u8，0–254 | 原样（HA 的 brightness 是 0–255） |
| 色温 | u16，mired | 原样 |

用整数存是为了避免在 C3 上做浮点、也避免 JSON 里出现
`23.100000000000001` 这种东西。转换只在序列化的最后一步做。
完整映射见 [reporting.md](reporting.md)。

---

## 4. 枚举值

### `en2m_lock_state_t`

| 名字 | 值 | 上报为 |
|---|---|---|
| `EN2M_LOCK_UNLOCKED` | 0 | `"UNLOCKED"` |
| `EN2M_LOCK_LOCKED` | 1 | `"LOCKED"` |

### `en2m_fan_mode_t`

| 名字 | 值 | 字符串 |
|---|---|---|
| `EN2M_FAN_OFF` | 0 | `"off"` |
| `EN2M_FAN_LOW` | 1 | `"low"` |
| `EN2M_FAN_MEDIUM` | 2 | `"medium"`（下行也接受 `"med"`） |
| `EN2M_FAN_HIGH` | 3 | `"high"` |
| `EN2M_FAN_ON` | 4 | `"on"` |
| `EN2M_FAN_AUTO` | 5 | `"auto"` |
| `EN2M_FAN_SMART` | 6 | `"smart"` |

无法识别的字符串一律解析成 `EN2M_FAN_OFF`。

### `en2m_thermostat_mode_t`

值照着 Matter 的 SystemMode 取，所以**不连续**：

| 名字 | 值 | 字符串 |
|---|---|---|
| `EN2M_THERMOSTAT_OFF` | 0 | `"off"` |
| `EN2M_THERMOSTAT_AUTO` | 1 | `"auto"` |
| `EN2M_THERMOSTAT_COOL` | 3 | `"cool"` |
| `EN2M_THERMOSTAT_HEAT` | 4 | `"heat"` |
| `EN2M_THERMOSTAT_FAN_ONLY` | 7 | `"fan_only"`（下行也接受 `"fan"`） |

无法识别的字符串一律解析成 `EN2M_THERMOSTAT_OFF`。

模式会影响 `target_temperature` 落到哪个 setpoint：**`cool` 落制冷 setpoint，
其余落制热 setpoint**。上报时也是按当前模式挑对应的那个 setpoint 报出去。

### `en2m_command_id_t`

| 名字 | 说明 |
|---|---|
| `EN2M_CMD_UNKNOWN` | 解析不出来。回调里可以看 `command->name` 自己判断 |
| `EN2M_CMD_OFF` / `_ON` / `_TOGGLE` | OnOff |
| `EN2M_CMD_MOVE_TO_LEVEL` | 参数 = 0–254 |
| `EN2M_CMD_MOVE_TO_COLOR_TEMPERATURE` | 参数 = mired |
| `EN2M_CMD_LOCK_DOOR` / `_UNLOCK_DOOR` | |
| `EN2M_CMD_UP_OR_OPEN` / `_DOWN_OR_CLOSE` / `_STOP_MOTION` | 窗帘 |
| `EN2M_CMD_GO_TO_LIFT_PERCENTAGE` | 参数 = 0–100 闭合百分比 |
| `EN2M_CMD_SET_FAN_MODE` / `_SET_FAN_PERCENT` | |
| `EN2M_CMD_SET_SYSTEM_MODE` | |
| `EN2M_CMD_SET_HEATING_SETPOINT` / `_SET_COOLING_SETPOINT` | 参数 = 0.01 °C |
| `EN2M_CMD_IDENTIFY` | 参数 = 秒 |
| `EN2M_CMD_WRITE_ATTRIBUTE` | 预留，当前解码器不产生它 |

---

## 5. 设备类型配方

`en2m_endpoint_create_device(id, type)` = `en2m_endpoint_create(id)` +
`en2m_endpoint_add_device_type(ep, type)`，一行把该有的 cluster 全建好。

| `en2m_device_type_t` | 建出来的 cluster | 对应 HA 实体 |
|---|---|---|
| `ON_OFF_LIGHT` | OnOff | `light` |
| `DIMMABLE_LIGHT` | OnOff + LevelControl | `light` |
| `COLOR_TEMPERATURE_LIGHT` | OnOff + LevelControl + ColorControl | `light` |
| `ON_OFF_PLUG` | OnOff | `switch` |
| `SMART_PLUG` | OnOff + ElectricalPower | `switch` + `sensor` ×2 |
| `CONTACT_SENSOR` | BooleanState | `binary_sensor` |
| `OCCUPANCY_SENSOR` | Occupancy | `binary_sensor` |
| `LIGHT_SENSOR` | Illuminance | `sensor` |
| `TEMPERATURE_SENSOR` | TemperatureMeasurement | `sensor` |
| `HUMIDITY_SENSOR` | RelativeHumidity | `sensor` |
| `PRESSURE_SENSOR` | PressureMeasurement | `sensor` |
| `SMOKE_CO_ALARM` | SmokeCO | `binary_sensor` ×2 |
| `FAN` | FanControl | `fan` |
| `WINDOW_COVERING` | WindowCovering | `cover` |
| `DOOR_LOCK` | DoorLock | `lock` |
| `THERMOSTAT` | Thermostat | `climate` |

### 配方可以叠加

`en2m_endpoint_add_device_type` 只是建 cluster，重复建同一个 cluster 会
返回已存在的那个。所以可以叠：

```c
en2m_endpoint_t *ep = en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR);
en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_HUMIDITY_SENSOR);
en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_PRESSURE_SENSOR);
/* 一个 endpoint 上的温湿气压三合一，就像 BME280 */
```

也可以直接加 cluster：

```c
en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);   /* 配方不会自动加 Identify */
```

**Identify cluster 不在任何配方里**，需要倒计时效果就自己建
（见 [state-flow.md](state-flow.md#9-identify-倒计时)）。

### 配方没覆盖的组合

想做一个"带亮度的灯 + 顺便测温"？直接拼：

```c
en2m_endpoint_t *ep = en2m_endpoint_create(1);
en2m_cluster_create(ep, EN2M_CLUSTER_ON_OFF);
en2m_cluster_create(ep, EN2M_CLUSTER_LEVEL_CONTROL);
en2m_cluster_create(ep, EN2M_CLUSTER_TEMPERATURE_MEASUREMENT);
```

`en2m_device_type_t` 只是常见组合的快捷方式，没有任何额外语义——
库里不存 "device type"，上报出去的 `caps` 是**从实际存在的 cluster 推出来的**。

---

## 6. 构建 API

### 顺序要求

```
① en2m_endpoint_create / _create_device
② en2m_cluster_create                      （_create_device 已经做了）
③ en2m_attribute_create                    （cluster 默认属性已经够用时可跳过）
④ en2m_cluster_set_write_cb / _read_cb / _command_cb   （可选）
⑤ en2m_start
```

①②③ **必须在 `en2m_start` 之前**。库不会拦你在之后建，但那样建出来的属性
不会被 NVS 回放（`en2m_dm_restore` 只在 start 里跑一次），也可能在上报中途
被加进去。④ 有锁保护，运行时改也安全。

### 各函数速览

```c
/* 建 endpoint。id 必须 1–254。重复建同一个 id 返回 NULL 并打 error。 */
en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id);
en2m_endpoint_t *en2m_endpoint_get(uint8_t endpoint_id);

/* 建 endpoint 并按配方填 cluster。 */
en2m_endpoint_t *en2m_endpoint_create_device(uint8_t endpoint_id, en2m_device_type_t type);
esp_err_t en2m_endpoint_add_device_type(en2m_endpoint_t *endpoint, en2m_device_type_t type);

/* 建 cluster，自动填默认属性。已存在则返回原来那个（不是 NULL）。 */
en2m_cluster_t *en2m_cluster_create(en2m_endpoint_t *endpoint, uint16_t cluster_id);
en2m_cluster_t *en2m_cluster_get(en2m_endpoint_t *endpoint, uint16_t cluster_id);

/* 手动加属性。已存在则返回 ESP_OK 且不改动。 */
esp_err_t en2m_attribute_create(en2m_cluster_t *cluster, uint16_t attribute_id,
                                en2m_value_t default_value, bool persist);

/* 按 cluster 注册回调，优先于设备级回调。 */
esp_err_t en2m_cluster_set_write_cb  (en2m_cluster_t *c, en2m_attribute_write_cb_t cb, void *ctx);
esp_err_t en2m_cluster_set_read_cb   (en2m_cluster_t *c, en2m_attribute_read_cb_t  cb, void *ctx);
esp_err_t en2m_cluster_set_command_cb(en2m_cluster_t *c, en2m_command_handler_t    cb, void *ctx);
```

### 返回 NULL 的情况都要处理

```c
en2m_endpoint_t *ep = en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
if (ep == NULL) {
    ESP_LOGE(TAG, "could not create the endpoint");
    return;
}
```

失败原因只有三种，而且都会在日志里说清楚：

| 日志 | 原因 |
|---|---|
| `invalid endpoint id 0` | id 是 0 或 255 |
| `endpoint 1 already exists` | 重复建 |
| `no free endpoint slot (EN2M_MAX_ENDPOINTS=4)` | 超容量，调 Kconfig |
| `endpoint 1 is full, cannot add cluster 0x0006` | cluster 超容量 |
| `cluster 0x0201 is full, cannot add attribute 0x001c` | 属性超容量 |

**注意 `en2m_cluster_create` 的返回值语义和 `en2m_endpoint_create` 不同**：
cluster 已存在时返回**已有的那个**（方便叠加配方），endpoint 已存在时返回 **NULL**。

---

## 7. 容量与内存

三个上限都是编译期定长数组，可通过 Kconfig 调：

| 宏 | 默认 | Kconfig | 范围 |
|---|---|---|---|
| `EN2M_MAX_ENDPOINTS` | 4 | `CONFIG_EN2M_MAX_ENDPOINTS` | 1–16 |
| `EN2M_MAX_CLUSTERS_PER_ENDPOINT` | 8 | `CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT` | 1–16 |
| `EN2M_MAX_ATTRIBUTES_PER_CLUSTER` | 6 | `CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER` | 1–16 |

存储是**完全静态**的三维数组，不管你实际建了多少，BSS 里的占用是固定的：

```
sizeof(en2m_attr_slot_t)  ≈ 16 字节
一个 cluster  = 3 个回调指针 + 3 个 ctx + id + endpoint_id + used
              + 6 × 16 ≈ 128 字节
一个 endpoint = 8 × 128 ≈ 1 KB
整棵树        = 4 × 1 KB ≈ 4 KB
```

默认配置下属性存储约 4 KB BSS。**用不到的槽位也占内存**，
所以一个简单的开关把上限调小是有意义的：

```
CONFIG_EN2M_MAX_ENDPOINTS=1
CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT=2
CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER=2
# → 约 0.06 KB，省下近 4 KB
```

但注意 `EN2M_MAX_CLUSTERS_PER_ENDPOINT` 还影响上报去重表的大小
（`en2m_report_build` 里两个 `EN2M_MAX_CLUSTERS_PER_ENDPOINT * EN2M_MAX_ENDPOINTS`
的栈数组），调大时要相应调大 `task_stack_size`。

### 属性数够不够

默认 6 个/cluster。实际用量：

| Cluster | 用了几个 |
|---|---|
| Thermostat | 4（最多的） |
| FanControl、SmokeCO、ElectricalPower | 2 |
| 其余全部 | 1 |

6 是留了余量的。要给某个 cluster 加自定义属性时也够用。

---

## 相关文档

- 属性怎么被读写、两条路径的区别 → [state-flow.md](state-flow.md#2-属性状态机)
- 回调怎么分发到具体的 cluster → [callbacks.md](callbacks.md)
- cluster 怎么变成 HA 的 JSON 键 → [reporting.md](reporting.md)
- 哪些属性进 NVS → [persistence.md](persistence.md)
- 调容量上限 → [kconfig.md](kconfig.md)

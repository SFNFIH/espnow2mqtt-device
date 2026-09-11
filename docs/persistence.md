# 持久化

执行器状态断电不能丢：一个开着的灯重启后应该还是开的，
一把锁着的门重启后不能变成开的。`en2m` 用 NVS 做这件事，
而且**恢复时会穿过 write 回调**，所以硬件也跟着回到断电前的状态。

## 1. 哪些属性会持久化

`en2m_cluster_create()` 自动建的默认属性里，**执行器状态默认 persist，
传感器读数默认不 persist**：

| Cluster | Attribute | 持久 | 为什么 |
|---|---|:-:|---|
| OnOff | `ON_OFF` | ✓ | 灯该保持开关状态 |
| LevelControl | `CURRENT_LEVEL` | ✓ | 亮度该保持 |
| ColorControl | `COLOR_TEMPERATURE_MIREDS` | ✓ | 色温该保持 |
| DoorLock | `LOCK_STATE` | ✓ | **安全相关**，默认 `LOCKED` |
| WindowCovering | `CURRENT_POSITION_LIFT_PERCENT` | ✓ | 位置该保持 |
| FanControl | `FAN_MODE` | ✓ | |
| FanControl | `PERCENT_SETTING` | ✓ | |
| Thermostat | `OCCUPIED_HEATING_SETPOINT` | ✓ | 用户设的目标温度 |
| Thermostat | `OCCUPIED_COOLING_SETPOINT` | ✓ | |
| Thermostat | `SYSTEM_MODE` | ✓ | |
| ElectricalPower | `ENERGY_MWH` | ✓ | **累计量**，丢了就断档 |
| Identify | `IDENTIFY_TIME` | | 临时效果 |
| Thermostat | `LOCAL_TEMPERATURE` | | 实测值，开机重新读 |
| BooleanState | `STATE_VALUE` | | 门磁，开机重新读 |
| Occupancy | `OCCUPANCY` | | 同上 |
| Illuminance / Temperature / Humidity / Pressure | `MEASURED_VALUE` | | 同上 |
| SmokeCO | `SMOKE_STATE` / `CO_STATE` | | 同上 |
| ElectricalPower | `ACTIVE_POWER_MW` | | 瞬时功率，开机重新测 |

**判断标准很简单：这个值的真相源在哪？**
在用户/HA 那边（"我要开灯"）就 persist；在物理世界那边（"现在 23 度"）就不 persist。
`ENERGY_MWH` 是个例外——它是累计量，真相源在设备自己的历史里。

## 2. 自己声明

```c
esp_err_t en2m_attribute_create(en2m_cluster_t *cluster, uint16_t attribute_id,
                                en2m_value_t default_value, bool persist);
```

最后那个参数就是。**只能在创建时指定，运行期不能改**。

```c
en2m_cluster_t *c = en2m_cluster_create(ep, EN2M_CLUSTER_ON_OFF);
en2m_attribute_create(c, 0x4003, en2m_enum8(1), true);   /* 自定义属性，持久化 */
```

`default_value` 的类型同时定死了属性的类型（见
[data-model.md](data-model.md#类型强制重要)）。

## 3. 存储格式

### NVS 命名空间

```c
#define EN2M_NVS_NAMESPACE "en2m_attr"
```

和 Wi-Fi、应用自己的 NVS 数据互不干扰。要清空所有属性状态：

```bash
idf.py erase-flash          # 清全部 NVS，包括 Wi-Fi 校准数据
```

或者在代码里：

```c
nvs_handle_t h;
if (nvs_open("en2m_attr", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}
```

### 键格式

```c
snprintf(out, 16, "%u_%04x_%04x", path->endpoint_id, path->cluster_id, path->attribute_id);
```

| 属性 | 键 |
|---|---|
| ep 1 / OnOff / OnOff | `1_0006_0000` |
| ep 1 / LevelControl / CurrentLevel | `1_0008_0000` |
| ep 1 / Thermostat / SystemMode | `1_0201_001c` |
| ep 2 / DoorLock / LockState | `2_0101_0000` |

最长是 `255_0b04_0011`（13 字符），装得进 `char[16]`，
也在 NVS 的 15 字符键名上限内。

### blob 布局

```c
typedef struct __attribute__((packed)) {
    uint8_t type;    /* en2m_val_type_t */
    int64_t raw;     /* en2m_value_as_int() 的结果 */
} en2m_persisted_t;
```

**9 字节**。所有类型都归一化成 `int64_t` 存，读回来时按 `type` 重新塑形：

```c
value = en2m_i64(stored.raw);
slot->value = en2m_value_coerce((en2m_val_type_t)stored.type, &value);
```

存结构体本身（而不是存 `en2m_value_t`）的原因是：
`en2m_value_t` 里有个联合体，布局会随编译器和目标变，
而 `{uint8_t, int64_t}` 加 `packed` 之后是确定的 9 字节。

### 空间占用

一个 persist 属性约 9 字节数据 + NVS 条目开销（约 32 字节/条），
所以一个色温灯（3 个 persist 属性）大约 120 字节。
默认 NVS 分区 24 KB，完全不是问题。

## 4. 刷盘时机

### 脏标记

值真的变了才置脏：

```c
if (!en2m_value_equal(&slot->value, &committed)) {
    slot->value = committed;
    slot->persist_dirty = slot->persist;    /* 不 persist 的属性永远不脏 */
    changed = true;
}
```

### 批量刷，每 5 秒最多一次

```c
#define EN2M_PERSIST_FLUSH_MS 5000

/* en2m_model_tick 里： */
if (now_ms - s_model.last_persist_ms >= EN2M_PERSIST_FLUSH_MS) {
    s_model.last_persist_ms = now_ms;
    en2m_dm_flush_persist();
}
```

`en2m_dm_flush_persist()` 遍历所有属性，只写 `persist_dirty` 的那些，
而且 **NVS 句柄是懒打开的**——一个属性都没脏时连 `nvs_open` 都不会调：

```c
if (!opened) {
    if (nvs_open(EN2M_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, attribute state will not survive a reboot");
        return;
    }
    opened = true;
}
```

最后只在真写过东西时才 `nvs_commit`。

### 为什么不是每次改都写

HA 的亮度滑条被拖过去，1 秒内可能产生几十次 `CURRENT_LEVEL` 变化。
每次都 `nvs_commit` 会很快磨损 flash（NVS 是 log-structured 的，
频繁提交会触发频繁的擦除周期）。

**代价**：掉电可能丢最近 5 秒内的变化。对家居场景这是完全可接受的折衷——
最坏情况是灯的亮度回到 5 秒前的值。

### `en2m_stop` 会补一次

```c
esp_err_t en2m_stop(void)
{
    s_model.started = false;
    en2m_dm_flush_persist();     /* 正常关机不丢 */
    en2m_mesh_deinit();
    ...
}
```

所以**受控**的重启（调 `en2m_stop` 之后 `esp_restart`）不会丢东西。
只有硬掉电才可能丢 5 秒。

想在关键时刻强制刷盘（比如进深睡之前），目前没有公开 API。
变通办法是调 `en2m_stop()`，或者接受 5 秒窗口。

## 5. 开机恢复

这是整套机制最有用的部分：**恢复会穿过 write 回调，所以硬件也跟着回位**。

```
en2m_start()
│
├─ en2m_mesh_init()      Wi-Fi / ESP-NOW / 任务
│
├─ en2m_dm_restore()
│    nvs_open("en2m_attr", NVS_READONLY)
│      打不开就直接返回（第一次开机、或者 NVS 被擦过）
│    持锁，遍历所有 persist 槽位：
│      nvs_get_blob(key, &stored, &size)
│      读不到 或 size != 9 → 跳过，保留默认值
│      slot->value = coerce(stored.type, i64(stored.raw))
│      slot->persist_dirty = false
│    解锁，nvs_close
│
├─ en2m_model_apply_persisted()
│    遍历所有 persist 属性：
│      持锁抄下 (path, value)，解锁
│      en2m_attribute_write(path, value)
│        → cluster write_cb → 设备 write_cb
│        → **你的驱动把硬件设成这个值**
│
├─ started = true
└─ EN2M_EVENT_STARTED
```

### 为什么用 `write` 而不是直接塞值

因为塞值只恢复了**软件状态**，硬件还是上电默认态。
那样的话 HA 显示"灯开着"，实际灯是灭的，直到用户操作一次才对上。

走 write 回调之后，`en2m_attribute_write` 会调你的驱动，
PWM / GPIO 被设成正确的值。**第一份状态上报之前硬件就已经对了。**

### 恢复时回调的上下文

`en2m_model_apply_persisted` 在 `en2m_start` 里被调用，
所以 write 回调跑在**调 `en2m_start` 的任务**上（通常是 `main`）。
这意味着：

- 回调里的硬件初始化必须**已经完成**，所以 `app_main` 里要先初始化驱动再 `en2m_start`
- 回调不能假设自己在 en2m 任务上

```c
void app_main(void)
{
    ESP_ERROR_CHECK(drv_gpio_relay_init(PIN_RELAY, true));   /* ① 先建硬件 */
    en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
    ESP_ERROR_CHECK(en2m_start(&cfg));                        /* ② 回放会调 write 回调 */
}
```

顺序反了的话，回放时驱动还没初始化，write 回调会失败
（然后属性也不会提交，等于恢复失败）。

### 恢复失败会怎样

write 回调返回非 `ESP_OK` 非 `ESP_ERR_NOT_SUPPORTED` 时：

- 打一条 `write ... rejected: <err>` 的 WARN
- **属性保持 NVS 里读出来的值**（因为 `en2m_dm_restore` 已经直接写进槽位了）
- 硬件保持上电默认态

也就是这种情况下软硬件会不一致。所以 write 回调在恢复路径上应该尽量宽容。

### 恢复时不会上报

`en2m_model_apply_persisted` 执行时 `s_model.started` **还是 false**，
所以 `en2m_model_on_change` 里的这个判断为假：

```c
if (s_model.started && (report_mode == DEFAULT || report_mode == ON_CHANGE_ONLY)) {
    en2m_report_schedule(0);
}
```

不会产生一堆恢复期的上报。但 `attribute_changed` 回调**会**被调
（它只要求 `configured`），所以如果你在 `attribute_changed` 里做本地控制，
要注意它在启动期就会被触发。

## 6. 一个完整的例子

`examples/door_lock` 演示了持久化的完整链路。锁状态**必须**在断电后保持——
一把锁着的门重启后变成开的是安全问题。

```c
#define ENDPOINT 1

static bool s_bolt_engaged;      /* 代表真实的锁舌 */

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id != EN2M_CLUSTER_DOOR_LOCK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_bolt_engaged = (value->v.e8 == EN2M_LOCK_LOCKED);
    ESP_LOGI(TAG, "锁舌 %s", s_bolt_engaged ? "伸出" : "缩回");
    return ESP_OK;                /* 真实驱动在这里动电机 */
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "lock1", .model = "ex-lock"},
        .attribute_write = on_write,
    };

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_DOOR_LOCK) == NULL) {
        return;
    }
    ESP_ERROR_CHECK(en2m_start(&cfg));
    /* en2m_start 返回时 on_write 已经被调过一次，s_bolt_engaged 是上次的状态 */
}
```

`LOCK_STATE` 的默认值是 `EN2M_LOCK_LOCKED`，所以**第一次开机**（NVS 里什么都没有）
时也是安全的默认态。

## 7. 排查

| 症状 | 原因 |
|---|---|
| 日志里 `nvs_open failed, attribute state will not survive a reboot` | NVS 分区满了，或者分区表里没有 `nvs`。检查 `partitions.csv` |
| 重启后状态没恢复 | ① 属性不是 persist；② 变化发生在最后 5 秒且是硬掉电；③ NVS 被擦过 |
| 重启后属性对了但硬件不对 | write 回调在恢复时失败了。看有没有 `rejected` 的 WARN，检查驱动初始化顺序 |
| 恢复的值不对（比如 200 变成 44） | 类型强制溢出。检查属性声明的类型够不够宽 |
| 想让某个执行器**不**恢复 | 用 `en2m_attribute_create(..., persist = false)` 手动建，覆盖 cluster 默认值 |

要覆盖 cluster 的默认 persist 设置，得**先建属性再建 cluster** 是做不到的
（`en2m_cluster_create` 会自动填默认属性）。实际做法是不用 `cluster_create`
的默认属性，而是接受它然后……目前**没有公开 API 改已存在属性的 persist 标志**。
如果确实需要不持久的 OnOff，只能改 `en2m_dm_add_default_attrs()`。

## 相关文档

- 持久化在启动流程里的位置 → [state-flow.md](state-flow.md#8-持久化生命周期)
- 哪些属性默认持久 → [data-model.md](data-model.md#3-cluster-与-attribute-全表)
- write 回调的契约 → [callbacks.md](callbacks.md#1-attribute_write--把值落到硬件)

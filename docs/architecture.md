# 架构总览

本篇讲 `en2m` 组件**怎么分层、每个文件负责什么、为什么这么设计**。
状态在这些层之间怎么流动，见 [state-flow.md](state-flow.md)。

## 1. 整体位置

`en2m` 只是整条链路的第一环。一个属性值从传感器到 Home Assistant 实体，要走完这些跳：

```
 ┌─ 本仓库 (espnow2mqtt-device) ────────────────┐
 │                                               │
 │   硬件 (WS2812 / 继电器 / AHT20 / PIR / 电机) │
 │        ↕  GPIO / I2C / RMT / PWM              │
 │   外设驱动 (注册表组件, 完全在 en2m 之外)     │
 │        ↕  回调 / en2m_attribute_set           │
 │   en2m 交互层 (en2m_model + en2m_datamodel)   │
 │        ↕  en2m_pkt_t                          │
 │   en2m 传输层 (en2m_mesh)                     │
 └───────────────┬───────────────────────────────┘
                 │  ESP-NOW，固定信道，树状 mesh
 ┌───────────────▼─── espnow2mqtt-host ─────────┐
 │   S3 协调器固件 (只用 en2m 的传输层)          │
 │        ↕  USB Serial/JTAG，NDJSON             │
 │   Python Bridge                               │
 └───────────────┬───────────────────────────────┘
                 │  MQTT
 ┌───────────────▼─── espnow2mqtt-ha ───────────┐
 │   HA 自定义集成 → light / sensor / cover ...  │
 └───────────────────────────────────────────────┘
```

关键点：**同一份 `components/en2m` 被两个仓库逐字节复用**。C3 设备用全部三层，
S3 协调器只用传输层（它没有 cluster，角色是 `EN2M_ROLE_COORDINATOR`）。

## 2. 三层分层

分层刻意照着 ESP-Matter 抄，边界也一样：

| 层 | 在组件里？ | 头文件 | 职责 |
|---|---|---|---|
| 硬件驱动 | **否** | — | GPIO / I2C / PWM / 一线。组件永远不碰外设 |
| 交互层（数据模型） | 是 | `en2m_model.h`、`en2m_attr.h` | endpoint / cluster / attribute、命令解码、上报策略、NVS 持久化、生命周期 |
| 事件 | 是 | `en2m_event.h` | 把所有异步事情发到 `esp_event` 默认循环 |
| 传输层 | 是 | `en2m_mesh.h`、`en2m_proto.h` | ESP-NOW 树状 mesh、选父、路由学习、转发、下行重传 |

**为什么驱动要留在应用里？** 因为外设的组合是无穷的。上一版库试图用 15 个
"driver ops" 结构体把驱动也吞进组件，结果每加一种硬件就要改库。现在组件只说
"把这个值落到硬件上"，怎么落是应用的事，库的表面积就稳定了。

## 3. 源文件职责

```
components/en2m/
├── include/
│   ├── en2m.h              汇总头，应用只 #include 这一个
│   ├── en2m_attr.h         值类型、cluster/attribute/command ID、五个回调 typedef、属性读写 API
│   ├── en2m_model.h         生命周期、endpoint/cluster 构建、上报、延迟执行
│   ├── en2m_event.h         事件 base、12 个事件 id、payload 结构、注册接口
│   ├── en2m_mesh.h          传输配置、角色、发包、mesh 状态查询、全部 Kconfig 默认值
│   ├── en2m_proto.h         en2m_pkt_t 空中帧、msg_type、MAC 辅助函数
│   └── en2m_config.h        兼容用的空壳头
├── src/
│   ├── en2m_datamodel.c     属性存储（831 行）
│   ├── en2m_model.c         生命周期 + 命令 + 上报（1160 行）
│   ├── en2m_mesh.c          mesh + 任务 + 队列（1009 行）
│   ├── en2m_event.c         esp_event 薄封装（48 行）
│   ├── en2m_mac.c           MAC 字符串互转（29 行）
│   └── en2m_priv.h          跨文件内部结构与钩子，非公开 API
├── Kconfig                 全部可调项
└── CMakeLists.txt          REQUIRES esp_wifi esp_common nvs_flash esp_event esp_netif esp_timer json
```

### `en2m_datamodel.c` — 属性存储

持有 `static struct en2m_endpoint s_endpoints[EN2M_MAX_ENDPOINTS]`，全部静态分配，
没有 `malloc`。一把静态互斥锁（`xSemaphoreCreateMutexStatic`）保护整棵树。

它提供的东西：

- 构建：`en2m_endpoint_create` / `en2m_cluster_create` / `en2m_attribute_create`
- 设备类型配方：`en2m_endpoint_add_device_type`（16 种）
- cluster 默认属性：`en2m_dm_add_default_attrs`（建 cluster 时自动填好该有的属性和默认值）
- 访问：`en2m_attribute_get` / `_set` / `_set_from_isr` / `_write`
- 16 个 `en2m_report_*` 便利包装
- 读刷新：`en2m_dm_refresh`（上报前调所有 read 回调）
- NVS：`en2m_dm_restore` / `en2m_dm_flush_persist`
- 值工具：`en2m_value_as_int` / `en2m_value_equal` / `en2m_value_coerce`

### `en2m_model.c` — 生命周期、命令、上报

持有 `static struct s_model`，里面是配置副本和四个时间戳（`last_report_ms`、
`next_report_ms`、`last_persist_ms`、`next_identify_ms`）。

- 生命周期：`en2m_start` / `en2m_stop`，含开机 NVS 回放
- 命令解码：扁平 HA 风格（`{"switch":"ON"}`）和 cluster 风格（`{"cluster":"on_off","command":"toggle"}`）两套
- 命令派发：cluster 回调 → 设备回调 → 内建翻译成属性写
- 上报序列化：`en2m_report_cluster` 把 cluster 翻译成扁平 HA 键
- 上报降级：`en2m_report_transmit` 三档降级，保证不发截断 JSON
- 周期工作：`en2m_model_tick`（identify 倒计时、NVS 刷盘、周期上报）
- 枚举与字符串互转：fan mode、hvac mode

### `en2m_mesh.c` — 传输、任务、队列

持有 `static en2m_ctx_t s_ctx`（邻居表、路由表、pending 表、锁、队列、任务句柄）。
这是**唯一创建任务的文件**。

- ESP-NOW 收包回调：只做一次 `xQueueSend`，别的什么都不干
- `en2m_task`：唯一的组件任务，排空队列 + 100 ms 维护 tick
- 选父：`en2m_consider_parent`
- 路由：`en2m_learn_route` / `en2m_lookup_route` / `en2m_expire_routes`
- 转发：`en2m_forward_toward_coord` / `en2m_forward_toward_dest`
- 下行重传：`en2m_pending_add` / `_resolve` / `_tick`
- 发包：`en2m_send_uplink` / `en2m_send_downlink`
- 延迟执行：`en2m_schedule` / `en2m_schedule_from_isr`
- 起停：`en2m_mesh_init` / `en2m_mesh_deinit`（Wi-Fi STA + ESP-NOW + 任务）

### `en2m_priv.h` — 内部契约

不对外。它定义了三件事：

1. 内部数据结构：`en2m_attr_slot_t`、`struct en2m_cluster`、`struct en2m_endpoint`、
   `en2m_neighbor_t`、`en2m_route_t`、`en2m_pending_t`、`en2m_item_t`、`en2m_ctx_t`
2. 数据模型暴露给 model 层的钩子：`en2m_dm_lock` / `_find_cluster` / `_find_attr` /
   `_refresh` / `_restore` / `_flush_persist`
3. mesh 与 model 互相调用的钩子：`en2m_task_post` / `_running` / `_is_current`、
   `en2m_model_config` / `_on_change` / `_on_command_frame` / `_tick` / `_on_link_change`

这套钩子是为了**避免循环依赖**：三个 .c 文件互相需要对方的一小块能力，
都通过 `en2m_priv.h` 里声明的函数走，不直接访问对方的 static 变量。

## 4. 关键设计决策

### 4.1 组件持有任务，应用不轮询

组件恰好创建**一个**任务，名叫 `en2m`。收包、维护、重传、读刷新、上报、
命令派发、全部应用回调都在它上面跑。

代价：多一个任务（默认 4096 字节栈）。换来的是应用侧零轮询、回调里可以阻塞、
可以走 I2C、可以写 flash——因为它不在 Wi-Fi 回调上下文里。详见 [concurrency.md](concurrency.md)。

### 4.2 一个队列，不是三个

收到的帧、ISR 延迟的工作、ISR 延迟的属性提交，全部走同一个
`QueueHandle_t s_ctx.queue`（`en2m_item_t`，带 tag 的联合体）。

这样做的理由是**保序**：如果按键中断和远程命令走两条队列，两者的相对顺序就没定义了。
同一个队列意味着"按键先按下，命令后到达"这个事实在库里是被保留的。

代价：每个槽位要放一个完整的 `en2m_pkt_t`（约 260 字节），所以 `EN2M_QUEUE_LEN`
默认只有 8。

### 4.3 收包回调只入队

```c
static void en2m_espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    /* ... 校验长度、拷贝 ... */
    if (s_ctx.queue == NULL || xQueueSend(s_ctx.queue, &item, 0) != pdTRUE) {
        s_ctx.rx_dropped++;
    }
}
```

`xQueueSend` 的超时是 **0**——收包回调跑在 Wi-Fi 任务上，绝不能阻塞。
队列满就丢包并累加计数器，维护 tick 会把它作为 `EN2M_EVENT_RX_DROPPED`
报出来并打一条警告。**丢包是可见的，不是静默的。**

### 4.4 回调在锁外调用

`en2m_handle_rx` 在持锁状态下运行，但它**不直接调应用回调**。它把"需要通知应用什么"
写进一个 `en2m_rx_outcome_t` 出参，由调用方 `en2m_dispatch_rx` 解锁之后再调：

```c
en2m_lock();
en2m_handle_rx(..., &outcome);
en2m_unlock();

if (outcome.parent_found) { en2m_event_post(...); en2m_model_on_link_change(true); }
if (outcome.deliver_command) { ... en2m_model_on_command_frame(...); }
```

不这么做就会死锁：命令回调里调 `en2m_attribute_write`，写回调里调 `en2m_report_now`，
一路又要拿 mesh 锁。同理，`en2m_dm_refresh` 也是"持锁取快照 → 解锁 → 调回调"。

### 4.5 两条属性访问路径，故意不等价

| | `en2m_attribute_set` | `en2m_attribute_write` |
|---|---|---|
| 用于 | 传感器 | 执行器 |
| 走 write 回调？ | **不走** | 走 |
| 提交时机 | 立即 | 仅当回调返回 `ESP_OK` |
| 语义 | "硬件现在就是这个值" | "请把硬件变成这个值" |

传感器的真相源在应用手里，让它再穿一遍 write 回调是荒谬的（而且会无限递归）。
执行器反过来：**回调失败就不提交、不上报**，所以一个卡住的继电器不会上报一个它没到达的状态。

### 4.6 上报降级，不截断

一个 ESP-NOW 帧的载荷上限是 `EN2M_DATA_MAX`（160 字节）。上一版代码在超长时
直接 `memcpy` 前 160 字节，于是发出的是**非法 JSON**，HA 那边解析失败。

现在按三档构建，发第一个装得下的：

| 档 | `node_role` | `caps` |
|---|---|---|
| 1 | 有 | 有 |
| 2 | 无 | 有 |
| 3 | 无 | 无 |

安全性来自一个事实：**host 端会合并同一设备的连续上报**
（HA 集成里 `merged = dict(dev.state); merged.update(payload)`），
所以某次上报省掉字段不会丢信息。三档都装不下时才打 error 并置 `truncated`。

### 4.7 三级 fall-through

写和命令都支持"逐级下沉"，用 `ESP_ERR_NOT_SUPPORTED` 表示"我不管，交给下一级"：

```
cluster 级回调  →  设备级回调  →  内建默认行为
```

这让"一个固件驱动多个互不相关的外设"变得干净：风扇 cluster 一个回调带自己的 ctx，
灯 cluster 另一个回调带另一个 ctx，不用写一个大 switch。
见 [callbacks.md](callbacks.md)。

### 4.8 静态分配

整个组件没有一次 `malloc`（cJSON 序列化除外）。endpoint 树、邻居表、路由表、
pending 表全是编译期定长数组，容量由 Kconfig 决定。好处是一块 C3 的堆不会被
mesh 状态吃掉，也不会有碎片化导致的运行时失败。

### 4.9 干净断裂，不做兼容层

从 driver-ops 版本升级时**没有**提供 driver-ops 兼容 shim。那 15 个结构体正是要被
消灭的东西，仓库还没到 1.0，留着只会让新 API 看起来像个补丁。
只保留了 `en2m_model_loop()` / `en2m_mesh_loop()` 两个**打一次警告的空函数**，
让旧固件还能编过。迁移对照表见 [migration.md](migration.md)。

## 5. 内存与体积

| 项 | 默认值 | 说明 |
|---|---|---|
| endpoint 树 | 4 × 8 × 6 属性 | 静态 BSS，约 1.5 KB |
| 队列 | 8 × ~264 B | 约 2.1 KB |
| 邻居表 | 16 × 24 B | 约 0.4 KB |
| 路由表 | 32 × 28 B | 约 0.9 KB |
| pending 表 | 4 × ~268 B | 约 1.1 KB，仅协调器实际用到 |
| en2m 任务栈 | 4096 B | `task_stack_size` 可调 |

示例固件实测 0xc5c90–0xc74d0（约 808–816 KB），1 MB app 分区剩 22–23%。
调参见 [kconfig.md](kconfig.md)。

## 6. 依赖

`CMakeLists.txt` 的 `REQUIRES`：

| 组件 | 用途 |
|---|---|
| `esp_wifi` | STA 模式 + 固定信道（不连 AP） |
| `esp_common` | `esp_err_t`、`esp_check.h` |
| `nvs_flash` | 属性持久化 |
| `esp_event` | 事件默认循环 |
| `esp_netif` | ESP-NOW 前置初始化 |
| `esp_timer` | 单调时间源（`esp_timer_get_time`） |
| `json` | cJSON，上报序列化与命令解析 |

没有依赖 `driver`——因为组件不碰 GPIO。示例需要 GPIO 时自己在
`idf_component_register(REQUIRES ...)` 里加 `driver`。

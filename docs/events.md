# 事件

组件里发生的每一件异步的事都会发到 **ESP-IDF 默认事件循环**，
所以应用可以**不轮询**地观察整个栈。

## 1. 注册

```c
#include "en2m.h"

static void on_en2m(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case EN2M_EVENT_PARENT_FOUND: {
        const en2m_event_parent_t *p = data;
        ESP_LOGI(TAG, "入网：cost=%u rssi=%d", p->cost, p->rssi);
        break;
    }
    case EN2M_EVENT_PARENT_LOST:
        ESP_LOGW(TAG, "掉线");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m, NULL));
    /* ... */
}
```

```c
/* 订阅单个事件，或 EN2M_EVENT_ANY 订阅全部 */
esp_err_t en2m_event_handler_register(int32_t event_id, esp_event_handler_t handler, void *handler_arg);
esp_err_t en2m_event_handler_unregister(int32_t event_id, esp_event_handler_t handler);
```

`en2m_event_handler_register` 会在应用还没建默认事件循环时**自动建一个**
（内部把 `esp_event_loop_create_default()` 的 `ESP_ERR_INVALID_STATE`
当成成功），所以**可以在 `en2m_start` 之前调**，不会漏掉
`EN2M_EVENT_STARTED`。

事件 base 是 `EN2M_EVENT`，用 `ESP_EVENT_DECLARE_BASE` 声明。
也可以直接用 ESP-IDF 原生接口注册：

```c
esp_event_handler_instance_register(EN2M_EVENT, EN2M_EVENT_PARENT_LOST,
                                    on_lost, NULL, NULL);
```

## 2. 运行上下文

处理函数**总是跑在默认事件循环任务上**，永远不在中断里、
也永远不在 ESP-NOW 收包回调里。所以处理函数可以：

- 调任意 `en2m_*` API
- 阻塞、走 I2C、写 flash

但事件循环任务是 ESP-IDF 的共享资源，**别长时间阻塞它**
（别的组件的事件也在那条队列上排着）。

事件投递是**异步**的：`en2m_event_post` 把 payload 拷进事件队列就返回，
处理函数稍后才跑。所以看到事件时，库里的状态可能已经又变了。

## 3. 事件全表

| 事件 | payload | 什么时候发 | 发在哪个上下文 |
|---|---|---|---|
| `EN2M_EVENT_STARTED` | 无 | `en2m_start` 成功的最后一步 | 调 `en2m_start` 的任务 |
| `EN2M_EVENT_STOPPED` | 无 | `en2m_stop` 完成 | 调 `en2m_stop` 的任务 |
| `EN2M_EVENT_PARENT_FOUND` | `en2m_event_parent_t` | 选上（或换到）一个父节点 | en2m 任务 |
| `EN2M_EVENT_PARENT_LOST` | `en2m_event_parent_t` | 父节点超过 `EN2M_PARENT_STALE_MS` 没发 beacon | en2m 任务 |
| `EN2M_EVENT_PAIRING_CHANGED` | `en2m_event_pairing_t` | `en2m_set_pairing()` 真的改变了状态 | 调用者任务 |
| `EN2M_EVENT_ATTRIBUTE_UPDATED` | `en2m_event_attribute_t` | 任何属性**真的变了** | 调 `set`/`write` 的任务 |
| `EN2M_EVENT_COMMAND_RECEIVED` | `en2m_event_command_t` | 一条命令派发前 | en2m 任务 |
| `EN2M_EVENT_REPORT_SENT` | `en2m_event_report_t` | 一份状态报文发出后 | en2m 任务 |
| `EN2M_EVENT_IDENTIFY` | `en2m_event_identify_t` | 收到 IDENTIFY 命令 | en2m 任务 |
| `EN2M_EVENT_ACK_RECEIVED` | `en2m_event_ack_t` | 下行被确认（**仅协调器**） | en2m 任务 |
| `EN2M_EVENT_ACK_TIMEOUT` | `en2m_event_ack_t` | 下行重传耗尽（**仅协调器**） | en2m 任务 |
| `EN2M_EVENT_RX_DROPPED` | `en2m_event_dropped_t` | 队列满导致丢帧，维护 tick 汇总上报 | en2m 任务 |

`EN2M_EVENT_ANY` 是 `ESP_EVENT_ANY_ID` 的别名。

## 4. payload 结构

```c
typedef struct {
    uint8_t mac[6];     /* 父节点 MAC；LOST 时是最后一个已知父节点 */
    uint8_t cost;       /* 本节点的 path_cost（父 cost + 1） */
    int8_t  rssi;       /* 触发这次事件的 beacon 的 RSSI；LOST 时为 0 */
} en2m_event_parent_t;

typedef struct {
    bool enabled;
} en2m_event_pairing_t;

typedef struct {
    en2m_attr_path_t path;    /* endpoint / cluster / attribute */
    en2m_value_t     value;   /* 提交后的新值，已经过类型强制 */
} en2m_event_attribute_t;

typedef struct {
    uint8_t  endpoint_id;
    uint16_t cluster_id;
    en2m_command_id_t id;
    uint16_t transaction_id;  /* 帧的 cmd_id，0 表示不要求确认 */
} en2m_event_command_t;

typedef struct {
    uint16_t  length;     /* 实际发出的 payload 字节数 */
    bool      truncated;  /* 三档降级都装不下，报文被截断了 */
    esp_err_t err;        /* en2m_send_uplink 的返回值 */
} en2m_event_report_t;

typedef struct {
    uint8_t  endpoint_id;
    uint16_t seconds;
} en2m_event_identify_t;

typedef struct {
    uint8_t  mac[6];
    uint16_t transaction_id;
    uint8_t  attempts;    /* 总共发了几次（首发算 1） */
} en2m_event_ack_t;

typedef struct {
    uint32_t total;       /* 本轮汇总里累计丢了多少帧 */
} en2m_event_dropped_t;
```

## 5. 逐个事件的注意点

### `STARTED` / `STOPPED`

`data` 是 `NULL`，不要解引用。

`STARTED` 发出时 mesh 已经初始化、en2m 任务已经在跑，但**很可能还没有父节点**。
"能通信了"要看 `PARENT_FOUND`。

### `PARENT_FOUND`

每次**换父**都会发，不只是第一次入网。所以可能连续收到多次
（比如一个 RSSI 更好的 router 出现了）。

不会在"父节点发来同 cost 的 beacon"时重复发——那条路径只是续命，
不算换父（见 [state-flow.md](state-flow.md#选父判定en2m_consider_parent)）。

### `PARENT_LOST`

`rssi` 字段固定是 `0`（丢的时候没有新 beacon 可参考），`mac` 和 `cost` 是
掉线前最后的值。

收到它之后：`en2m_send_uplink` 对 STATE 帧会返回 `ESP_ERR_INVALID_STATE`，
也就是**上报会失败**直到重新入网。属性存储照常工作，
重新入网后 200 ms 内会自动补一份完整状态。

### `PAIRING_CHANGED`

只在状态**真的翻转**时发。连续两次 `en2m_set_pairing(true)` 只发一次。
主要给协调器固件用（点亮配网指示灯之类）。

### `ATTRIBUTE_UPDATED`

和 `attribute_changed` 回调携带**完全相同**的信息。区别：

| | `attribute_changed` 回调 | `ATTRIBUTE_UPDATED` 事件 |
|---|---|---|
| 上下文 | 调用者任务，**同步** | 事件循环任务，**异步** |
| 订阅者 | 一个 | 任意多个 |
| 时机 | 提交后立刻 | 稍后 |
| 适合 | 需要立即反应的本地控制 | 日志、统计、松耦合模块 |

要做本地控制闭环用回调；要做旁路观察用事件。

### `COMMAND_RECEIVED`

在派发**之前**发，所以收到它不代表命令成功了。想知道结果就看
后续的 `ATTRIBUTE_UPDATED`。

`transaction_id` 是帧里的 `cmd_id`，和协调器那边的 pending 条目一一对应。

### `REPORT_SENT`

最有用的诊断事件。三个字段都要看：

```c
case EN2M_EVENT_REPORT_SENT: {
    const en2m_event_report_t *r = data;
    if (r->err != ESP_OK) {
        ESP_LOGW(TAG, "上报失败 %s", esp_err_to_name(r->err));   /* 常见：没父节点 */
    } else if (r->truncated) {
        ESP_LOGE(TAG, "报文超长被截断，得拆 cluster");
    } else {
        ESP_LOGD(TAG, "上报 %u 字节", r->length);
    }
    break;
}
```

`err` 的常见取值见 [reporting.md](reporting.md#怎么知道自己被截断了)。

### `IDENTIFY`

只在**收到命令那一刻**发一次，`seconds` 是请求的完整秒数。
**每秒的倒计时不发事件**，只调 `identify` 回调。
要跟着倒计时就用回调。

### `ACK_RECEIVED` / `ACK_TIMEOUT`

**只有协调器角色会发**。设备侧永远收不到（设备是回 ACK 的那一方，
而且回 ACK 是自动的，不产生事件）。

`attempts` 是总发送次数，**首发算 1**。默认参数下超时时是 `4`
（1 首发 + 3 重传）。`attempts > 1` 说明链路在丢包，可以据此判断信号质量。

协调器固件把 `ACK_TIMEOUT` 翻译成 USB 上的：

```json
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":false,"error":"timeout"}
```

### `RX_DROPPED`

队列满了。`total` 是**本轮汇总**的累计值——维护 tick 发完这个事件后
会把内部计数器清零，所以每次收到的 `total` 是上次汇总以来新丢的数量。

偶尔出现 1–2 帧不用管。持续出现说明：

| 原因 | 对策 |
|---|---|
| 回调阻塞太久，en2m 任务排不动队 | 缩短回调，或把慢活挪到自己的任务 |
| `en2m_schedule_from_isr` 调用太频繁（按键没消抖） | 在 ISR 里做消抖 |
| 空口太吵，帧率超过处理能力 | 调大 `CONFIG_EN2M_QUEUE_LEN` |

## 6. 一个完整的诊断处理函数

抄这段进去，设备行为立刻变得可观测：

```c
static void on_en2m_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case EN2M_EVENT_STARTED:
        ESP_LOGI(TAG, "en2m 启动");
        break;

    case EN2M_EVENT_PARENT_FOUND: {
        const en2m_event_parent_t *p = data;
        char mac[18];
        en2m_mac_to_str(p->mac, mac);
        ESP_LOGI(TAG, "入网 parent=%s cost=%u rssi=%d", mac, p->cost, p->rssi);
        break;
    }

    case EN2M_EVENT_PARENT_LOST:
        ESP_LOGW(TAG, "父节点丢失，正在重新搜索");
        break;

    case EN2M_EVENT_ATTRIBUTE_UPDATED: {
        const en2m_event_attribute_t *a = data;
        ESP_LOGD(TAG, "ep%u/0x%04x/0x%04x = %lld", a->path.endpoint_id,
                 a->path.cluster_id, a->path.attribute_id,
                 (long long)en2m_value_as_int(&a->value));
        break;
    }

    case EN2M_EVENT_COMMAND_RECEIVED: {
        const en2m_event_command_t *c = data;
        ESP_LOGI(TAG, "命令 id=%d cluster=0x%04x tid=%u", c->id, c->cluster_id,
                 c->transaction_id);
        break;
    }

    case EN2M_EVENT_REPORT_SENT: {
        const en2m_event_report_t *r = data;
        if (r->err != ESP_OK) {
            ESP_LOGW(TAG, "上报失败: %s", esp_err_to_name(r->err));
        } else if (r->truncated) {
            ESP_LOGE(TAG, "报文被截断（%u 字节）", r->length);
        }
        break;
    }

    case EN2M_EVENT_RX_DROPPED: {
        const en2m_event_dropped_t *d = data;
        ESP_LOGW(TAG, "丢了 %u 帧，队列跟不上", (unsigned)d->total);
        break;
    }

    default:
        break;
    }
}
```

`firmware/router` 就是一个只有事件处理函数、没有任何循环的固件：

```c
void app_main(void)
{
    en2m_config_t cfg = {.role = EN2M_ROLE_ROUTER, .name = "router1", ...};

    ESP_ERROR_CHECK(en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m_event, NULL));
    ESP_ERROR_CHECK(en2m_mesh_init(&cfg));
    /* app_main 返回，之后全靠事件 */
}
```

## 相关文档

- 每个事件对应的状态机 → [state-flow.md](state-flow.md)
- 事件循环任务的定位 → [concurrency.md](concurrency.md#1-有哪些执行上下文)
- 回调和事件怎么选 → [callbacks.md](callbacks.md#和事件的区别)

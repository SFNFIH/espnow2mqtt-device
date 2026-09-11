# Mesh 传输层

`en2m_mesh` 是一棵**以 USB 协调器为根的树**，跑在 ESP-NOW 上。
它不连家庭 Wi-Fi，只把射频锁在一个固定信道上收发。

本篇替代了旧的 `mesh-roles.md`。

- [1. 为什么是树，不是网](#1-为什么是树不是网)
- [2. 三种角色](#2-三种角色)
- [3. 空中帧格式](#3-空中帧格式)
- [4. 六种消息类型](#4-六种消息类型)
- [5. Beacon 与选父](#5-beacon-与选父)
- [6. 路由学习与转发](#6-路由学习与转发)
- [7. 配网窗口](#7-配网窗口)
- [8. 心跳与离线判定](#8-心跳与离线判定)
- [9. 可靠性](#9-可靠性)
- [10. 信道与 Wi-Fi 共存](#10-信道与-wi-fi-共存)
- [11. 只用传输层](#11-只用传输层)

---

## 1. 为什么是树，不是网

流量模式决定了拓扑。这套系统里几乎所有数据都是：

- **上行**：设备 → 协调器（状态上报、心跳、ACK）
- **下行**：协调器 → 某个设备（命令）

**没有设备之间的横向通信**。所以一棵以协调器为根的树就够了，
而且比全网状便宜得多：每个节点只需要记住"我的父是谁"，
不需要维护到所有节点的路径。

代价是协调器是单点。它掉了整个网就哑了——但这本来就是事实，
因为协调器就是那根 USB 棒。

---

## 2. 三种角色

角色在 `en2m_mesh_init` 时选定，运行期不变：

```c
en2m_config_t cfg = {
    .role = EN2M_ROLE_LEAF,          /* 或 ROUTER / COORDINATOR */
    .name = "leaf1",                  /* MQTT slug，最长 15 字符 */
    .model = "ex-th",                 /* 型号，最长 11 字符 */
    .fw = EN2M_FW_VERSION,            /* 可选 */
    .channel = 0,                     /* 0 = 用 EN2M_WIFI_CHANNEL */
};
```

| | Coordinator | Router | Leaf |
|---|---|---|---|
| `path_cost` | 固定 **0** | 父 cost + 1 | 父 cost + 1 |
| 选父 | **不选**，自己是根 | 选 | 选 |
| 发 beacon | 发，`cost = 0` | **有父才发**，`cost = path_cost` | **不发** |
| 转发上行 | — | → 父（`en2m_forward_toward_coord`） | 不转发 |
| 转发下行 | 发起 | 按路由表（`en2m_forward_toward_dest`） | 不转发 |
| 接受上行 | 接受（受配网/路由约束） | 只转发不消费 | — |
| 发下行 | 可以（`en2m_send_downlink`） | 不可以（返回 `NOT_SUPPORTED`） | 不可以 |
| 发上行 | **不可以**（返回 `NOT_SUPPORTED`） | 可以 | 可以 |
| 供电 | USB | 常电 | 可电池 |
| 本仓库里 | 在 host 仓库 | `firmware/router` | `examples/*` |

### Leaf 不发 beacon 的后果

Leaf **不能当别人的父**。所以拓扑只可能是：

```
协调器
├── leaf                       （1 跳）
├── router ── leaf             （2 跳）
└── router ── router ── leaf   （3 跳）
```

深度上限是 `EN2M_HOP_LIMIT`（默认 8）。

### 什么时候需要 router

只有一种情况：**有设备离协调器太远，单跳收不到**。
加一个常电的 router 放在中间。Router 固件极简：

```c
void app_main(void)
{
    en2m_config_t cfg = {
        .role = EN2M_ROLE_ROUTER,
        .name = "router1",
        .model = "ex-router",
        .fw = EN2M_FW_VERSION,
    };
    ESP_ERROR_CHECK(en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m_event, NULL));
    ESP_ERROR_CHECK(en2m_mesh_init(&cfg));
    /* 返回。Router 没有 cluster，不需要 en2m_start */
}
```

Router 直接调 `en2m_mesh_init` 而不是 `en2m_start`，因为它没有数据模型。

---

## 3. 空中帧格式

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;        /* 0xA5，EN2M_MAGIC */
    uint8_t  version;      /* 2，EN2M_VERSION */
    uint8_t  msg_type;     /* en2m_msg_type_t */
    uint8_t  role;         /* 发送者的角色 */
    uint8_t  hop;          /* 已经过几跳，每次转发 +1 */
    uint8_t  hop_limit;    /* EN2M_HOP_LIMIT */
    uint8_t  cost;         /* 发送者的 path_cost */
    uint8_t  flags;        /* EN2M_FLAG_PAIRING = 0x01 */
    uint32_t seq;          /* 发送者的自增序号 */
    uint16_t cmd_id;       /* 事务 id，0 = 不要求确认 */
    uint8_t  origin[6];    /* 原始发起者 MAC，转发时不变 */
    uint8_t  dest[6];      /* 目标 MAC；上行是广播地址 */
    uint8_t  via[6];       /* 上一跳 MAC，每次转发改写 */
    char     model[12];
    char     name[16];
    uint8_t  data_len;
    uint8_t  data[160];    /* EN2M_DATA_MAX */
} en2m_pkt_t;

_Static_assert(sizeof(en2m_pkt_t) <= 250, "ESP-NOW packet too large");
```

固定 **218 字节头 + 载荷**，总共正好卡在 ESP-NOW 的 250 字节上限内。
`_Static_assert` 保证改结构体时不会悄悄超限。

### 三个 MAC 字段的分工

| 字段 | 转发时 | 用途 |
|---|---|---|
| `origin` | **不变** | 谁发起的。协调器用它建路由、认设备 |
| `dest` | **不变** | 发给谁。上行填广播地址（因为目标就是"协调器方向"） |
| `via` | **每跳改写** | 上一跳是谁。协调器用它建反向路由、报给 host 做拓扑显示 |

`hop` 和 `via` 是**协调器加的**，设备自己不填这些（设备发的时候
`hop = 0`、`via = self`）。所以 host 看到的 `hop`/`via` 反映的是真实路径。

### `name` 和 `model` 的长度

```c
char model[12];   /* 有效字符最多 11，最后一字节留给 '\0' */
char name[16];    /* 有效字符最多 15 */
```

用 `strncpy(..., sizeof(x) - 1)` 拷入，超长**静默截断**。
`name` 会变成 MQTT topic 里的 slug，所以起名时注意别超过 15 字符。

---

## 4. 六种消息类型

```c
typedef enum {
    EN2M_MSG_BEACON    = 1,
    EN2M_MSG_HELLO     = 2,
    EN2M_MSG_STATE     = 3,
    EN2M_MSG_CMD       = 4,
    EN2M_MSG_ACK       = 5,
    EN2M_MSG_HEARTBEAT = 6,
} en2m_msg_type_t;
```

| 类型 | 方向 | 谁发 | 载荷 | 作用 |
|---|---|---|---|---|
| `BEACON` | 广播 | Coordinator、Router | 无 | 宣告"我可以当父，我的 cost 是 n"，同时带配网标志 |
| `HELLO` | 上行 | Leaf、Router | 无 | 自我介绍。**无父时也会广播** |
| `STATE` | 上行 | Leaf、Router | 状态 JSON | 状态上报。**无父时直接失败，不广播** |
| `CMD` | 下行 | Coordinator | 命令 JSON | 命令 |
| `ACK` | 上行 | Leaf、Router | 无 | 确认收到某个 `cmd_id` |
| `HEARTBEAT` | 上行 | Leaf、Router | 无 | 保活。**无父时也会广播** |

### 为什么 HELLO/HEARTBEAT 无父时广播，STATE 不广播

```c
if (!s_ctx.has_parent && msg_type != EN2M_MSG_HELLO && msg_type != EN2M_MSG_HEARTBEAT) {
    return ESP_ERR_INVALID_STATE;
}
```

HELLO 和 HEARTBEAT 是**无载荷的存在性声明**，广播出去让任何听到的
协调器/router 都能知道"这附近有个节点"，代价只有一帧。
STATE 带 160 字节载荷，无父时广播纯属浪费空口——而且反正 200 ms 后
选上父就会自动补发（见 [state-flow.md](state-flow.md#为什么第一份上报要等-500-ms)）。

---

## 5. Beacon 与选父

### Beacon 发送

在 100 ms 维护 tick 里判定，每 `EN2M_BEACON_INTERVAL_MS`（默认 5000 ms）一次：

```c
if (s_ctx.config.role != EN2M_ROLE_LEAF &&
    (now_ms - s_ctx.last_beacon_us / 1000) >= EN2M_BEACON_MS_DEFAULT) {
    beacon_due = true;
}
```

Router 有一条额外约束：**自己没有父就不发 beacon**。否则会形成
"两个都没入网的 router 互相当父"的孤岛。

Beacon 内容：`cost`（协调器 0，router 是自己的 `path_cost`）、
`role`、`origin = self`、`dest = 广播`，以及配网中时的 `EN2M_FLAG_PAIRING`。

### 选父算法

每收到一个 BEACON 都跑一遍 `en2m_consider_parent`：

```
① 自己是 COORDINATOR                  → 不选父
② 发送方 role 不是 COORD/ROUTER        → 拒绝（Leaf 不能当父）
③ 对方 cost >= 254                     → 拒绝（对方自己都没入网）
④ new_cost = 对方 cost + 1
   new_cost > EN2M_HOP_LIMIT (8)       → 拒绝（太深）
⑤ 是否更好？
   ├─ 当前无父                              → 更好
   ├─ new_cost < 当前 path_cost              → 更好（更近的路）
   ├─ new_cost == path_cost 且就是当前父     → 只刷新 parent_last_us，返回 false
   │                                            ← 这是心跳续命的主路径
   └─ new_cost == path_cost 且是另一个节点   → 仅当 rssi > 当前父 rssi + 8
                                                才算更好（8 dB 滞回）
⑥ 更好 → 记 parent_mac / path_cost / parent_last_us
         把父加成 ESP-NOW peer，打日志 "parent=XX:.. cost=n rssi=m"
         返回 true → EN2M_EVENT_PARENT_FOUND
```

### 8 dB 滞回是干什么的

两个 cost 相同的 router，信号强度在临界点附近抖动时，没有滞回的话
节点会不停地在两者之间来回切换（**父节点乒乓**），每次切换都要重建 peer、
重发状态。要求新候选比现任**强 8 dB 以上**才换，这个问题就消失了。

对比的是**邻居表里记的当前父的 RSSI**，邻居表里没有时按 `-100 dBm` 算
（也就是必然换）。

### 掉线判定

维护 tick 里：

```c
if (role != COORDINATOR && has_parent &&
    (now_ms - parent_last_us / 1000) > EN2M_PARENT_STALE_MS) {
    has_parent = false;
    path_cost = 255;
    /* 日志 "parent stale" → EN2M_EVENT_PARENT_LOST */
}
```

默认 beacon 5 秒、stale 20 秒，也就是**连丢 4 个 beacon 才判掉线**。

### 一个不明显的行为：父 cost 升高不续命

第 ⑤ 步里只有 `new_cost <= path_cost` 的分支会刷新 `parent_last_us`。
如果父节点自己掉了一层（它的 cost 从 1 变成 2），子节点收到的
`new_cost` 从 2 变成 3，大于自己的 `path_cost = 2`，于是：

- 不采纳（不是"更好"）
- **也不续命**
- 20 秒后超时 → `PARENT_LOST` → 重新选父，这次会接受 cost 3

看起来像个疏漏，其实是有意的：与其一直挂在一条正在变差的路径上，
不如短暂断开、重新评估所有候选。20 秒的窗口也给了父节点恢复的机会。

---

## 6. 路由学习与转发

### 反向路由表

协调器和 router 需要知道"要发给设备 X，下一跳是谁"。这个信息是从
**上行帧里学来的**：

```c
static void en2m_learn_route(const uint8_t origin[6], const uint8_t from[6], uint8_t hop)
{
    /* origin 就是自己 → 不学 */
    /* 已有该 origin 的条目：hop <= 现有 hop 才更新 next_hop，last_us 总是刷新 */
    /* 没有 → 占一个空槽 */
}
```

条目结构：

```c
typedef struct {
    bool used;
    uint8_t dest[6];       /* 帧里的 origin */
    uint8_t next_hop[6];   /* 帧里的 via（上一跳） */
    uint8_t hop;           /* 距离，用来比较哪条路更短 */
    int64_t last_us;
} en2m_route_t;
```

容量 `EN2M_MAX_ROUTES`（默认 32）。**满了就不再学新的**，不淘汰旧的——
所以 32 是这套系统的实际设备数上限，要更多就调 Kconfig。

### 老化

维护 tick 里：

| 表 | 老化阈值 | 默认 |
|---|---|---|
| 路由 | `EN2M_ROUTE_STALE_MS` | 120 秒 |
| 邻居 | `2 × EN2M_PARENT_STALE_MS` | 40 秒 |

路由每次收到该设备的上行帧就刷新。心跳 30 秒一次，所以**正常工作的设备
不会掉路由**。但**离线超过 120 秒的设备会掉路由**，回来时需要重新开配网
（见第 7 节）。

### 转发上行

```c
static void en2m_forward_toward_coord(en2m_pkt_t *pkt)
{
    if (role != ROUTER || !has_parent) return;
    if (pkt->hop >= pkt->hop_limit) return;      /* 环路保护 */
    pkt->hop++;
    en2m_mac_copy(pkt->via, self_mac);           /* 改写上一跳 */
    en2m_send_raw(parent_mac, pkt);              /* 无脑发给父 */
}
```

上行不需要路由表——"往协调器方向"就是"发给我的父"。

### 转发下行

```c
static void en2m_forward_toward_dest(en2m_pkt_t *pkt)
{
    if (role == LEAF) return;
    if (pkt->hop >= pkt->hop_limit) return;
    if (!en2m_lookup_route(pkt->dest, next)) {
        en2m_mac_copy(next, pkt->dest);          /* 没路由就直发目标 */
    }
    pkt->hop++;
    en2m_mac_copy(pkt->via, self_mac);
    en2m_send_raw(next, pkt);
}
```

**没有路由条目时会直发目标 MAC**。ESP-NOW 是链路层协议，
如果目标其实在射频范围内，这一发就成功了。这个回退让"路由还没学到"
的窗口期也能工作。

### 环路保护

唯一的机制是 `hop >= hop_limit` 就丢。没有序号去重，
所以**理论上**一个畸形的拓扑可以让一帧被放大 `hop_limit` 倍。
实际上树状拓扑不会出环（每个节点只有一个父），
`hop_limit` 只是兜底。

---

## 7. 配网窗口

协调器默认**不接受陌生设备的上行**：

```c
known = en2m_lookup_route(in->origin, existing_next);
if (!known && !s_ctx.pairing) {
    en2m_emit_log("drop uplink (not pairing / unknown)");
    return;
}
```

所以一个新设备要进网，必须在**配网窗口开着**的时候发上行。

### 怎么开

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60
```

链路：MQTT → Bridge → USB `{"type":"pair","seconds":60}` → 协调器
`en2m_set_pairing(true)` + 记下截止时间 → 协调器的 1 秒 housekeeping
定时器到期时 `en2m_set_pairing(false)` 并发 `pairing_disabled`。

`seconds` 被协调器 clamp 到 **1–300**。

### 配网标志会广播出去

`EN2M_FLAG_PAIRING` 会被塞进 beacon 的 `flags`，所以设备侧**能知道**
现在在配网。当前的设备固件没有用这个信息（它无条件发 HELLO/HEARTBEAT），
但这个位是给"只在配网时才尝试入网"的省电设备预留的。

### 一旦入网就不需要再配网

因为路由条目建立了，`known` 为真。之后即使断电重启也能直接回来——
只要**不超过 `EN2M_ROUTE_STALE_MS`（120 秒）**。

### 离线超过 2 分钟的设备回来会怎样

路由已经老化掉了，`known` 为假，配网窗口也关了，所以它的上行会被丢弃，
日志里出现 `drop uplink (not pairing / unknown)`。

**对策**：

| 方案 | 做法 |
|---|---|
| 临时 | 重新 `permit_join` |
| 长期 | 把 `CONFIG_EN2M_ROUTE_STALE_MS` 调大（比如 86400000 = 1 天） |

注意协调器固件自己的 peer 表用的是另一个阈值
`EN2M_OFFLINE_MS`（90 秒），到期只是向 host 报一条
`{"type":"device","event":"offline"}`，**不影响**重新接受上行。
真正卡住重连的是路由表的 120 秒。

### 踢掉一个设备

```bash
mosquitto_pub -t espnow2mqtt/<slug>/... # 见 host 仓库
```

USB 层是 `{"type":"unpair","mac":"..."}`，协调器会
`en2m_forget_route(mac)` 并从 peer 表移除。

---

## 8. 心跳与离线判定

三个不同的超时经常被搞混，这里列清：

| 常量 | 默认 | 谁在用 | 作用 |
|---|---|---|---|
| `EN2M_BEACON_INTERVAL_MS` | 5 s | Coordinator/Router 发 | 让子节点续命 |
| `EN2M_PARENT_STALE_MS` | 20 s | Leaf/Router 判自己掉线 | 触发 `PARENT_LOST` + 重选父 |
| `EN2M_HEARTBEAT_MS` | 30 s | Leaf/Router 发 HEARTBEAT | 刷新协调器的 last-seen 和路由 |
| `EN2M_OFFLINE_MS` | 90 s | **协调器固件**（不在组件里） | 向 host 报 `device.offline` |
| `EN2M_ROUTE_STALE_MS` | 120 s | Coordinator/Router 的路由表 | 老化路由，影响能否免配网重连 |

时间线上的合理性：beacon 5 s ≪ parent stale 20 s（容忍丢 4 个），
heartbeat 30 s ≪ offline 90 s（容忍丢 2 个），
offline 90 s < route stale 120 s（先报离线，再掉路由）。

---

## 9. 可靠性

ESP-NOW 本身在 MAC 层有 ACK 和重传，但那只保证**单跳**送达，
不保证多跳、也不保证应用层真的处理了。所以组件在应用层加了一层确认。

### 只有下行有确认

| 方向 | 确认？ | 理由 |
|---|:-:|---|
| 下行（CMD） | ✓ | 命令必须送到，用户点了开关就得动 |
| 上行（STATE） | ✗ | 状态是**幂等**的。丢了一份，下一份周期上报就补上了 |
| 上行（HEARTBEAT） | ✗ | 同理 |

给上行加确认会让每次上报变成两帧，而收益接近零——状态本来就在重复发。

### 下行确认的机制

完整的状态机和时间线见
[state-flow.md](state-flow.md#7-下行-ack--重传状态机)。要点：

- 协调器侧每个未确认下行占一个 `en2m_pending_t`（默认 4 个槽）
- 默认 400 ms 重传一次，最多 3 次，约 1.6 秒后判超时
- 设备侧**自动**回 ACK（只要 `cmd_id != 0` 且 JSON 解析成功），不需要应用参与
- 结果通过 `EN2M_EVENT_ACK_RECEIVED` / `EN2M_EVENT_ACK_TIMEOUT` 暴露

### 可调项

```c
en2m_config_t cfg = {
    .max_retries = 5,           /* 0 = 用 EN2M_CMD_RETRIES (3) */
    .retry_interval_ms = 600,   /* 0 = 用 EN2M_CMD_RETRY_MS (400) */
};
```

多跳网络适合调大间隔（一帧要走两跳，400 ms 可能偏紧）。

---

## 10. 信道与 Wi-Fi 共存

### 全网必须同一个信道

```c
#define EN2M_WIFI_CHANNEL 1     /* CONFIG_EN2M_WIFI_CHANNEL */
```

ESP-NOW 收发双方必须在同一信道。组件启动时：

```c
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();
esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
```

**只开 STA、不连任何 AP**、锁死信道。`esp_wifi_set_storage(WIFI_STORAGE_RAM)`
保证不会去 NVS 里读上次连过的 AP 而自动重连。

### 怎么选信道

选和家里 Wi-Fi **相同**的那个信道，或者离得最远的那个。

| 策略 | 效果 |
|---|---|
| 和家里 AP 同信道 | 遵守同一套 CSMA 退避，互相礼让，**通常最稳** |
| 完全不同的信道（1 vs 11） | 没有退避协作，但物理上不重叠，也可以 |
| 部分重叠（1 vs 3） | **最差**，互相当噪声 |

2.4 GHz 只有 1/6/11 三个互不重叠的信道，所以实际就在这三个里选。

### 单独改某个节点的信道

```c
en2m_config_t cfg = { .channel = 6 };   /* 覆盖 Kconfig 默认值 */
```

但记住**全网要一致**，包括协调器。改了协调器就得重刷所有设备。

---

## 11. 只用传输层

协调器和纯 router 没有 cluster，直接用传输层：

```c
#include "en2m.h"

static void on_uplink(const en2m_pkt_t *pkt, int8_t rssi, const uint8_t from[6], void *ctx)
{
    /* 协调器：把帧交给 host。pkt->data 是设备发来的 JSON */
}

void app_main(void)
{
    en2m_config_t cfg = {
        .role = EN2M_ROLE_COORDINATOR,
        .name = "coordinator",
        .model = "s3-coord",
        .on_uplink = on_uplink,
        .on_log = on_mesh_log,
    };
    ESP_ERROR_CHECK(en2m_mesh_init(&cfg));   /* 不是 en2m_start */
}
```

### 三个传输层回调

```c
/* 协调器：收到需要交给 host 的上行帧 */
typedef void (*en2m_uplink_cb_t)(const en2m_pkt_t *pkt, int8_t rssi,
                                 const uint8_t from_mac[6], void *user_ctx);

/* 收到发给本节点的裸 CMD 帧。为 NULL 时交互层装自己的解码器 */
typedef void (*en2m_frame_cb_t)(const en2m_pkt_t *pkt, void *user_ctx);

/* 日志出口。为 NULL 时走 ESP_LOG */
typedef void (*en2m_log_cb_t)(const char *msg, void *user_ctx);
```

`on_command` 是个逃生口：设置了它就**完全接管**命令解析，
`en2m_model_on_command_frame` 不会被调用，也就没有自动 ACK、
没有内建翻译、没有自动上报。只有在你要实现一套完全不同的
payload 协议时才用它。

### mesh 状态查询

```c
bool     en2m_has_parent(void);
uint8_t  en2m_get_path_cost(void);      /* 255 = 无父 */
en2m_role_t en2m_get_role(void);
void     en2m_get_parent_mac(uint8_t out[6]);   /* 无父时填广播地址 */
void     en2m_get_self_mac(uint8_t out[6]);
bool     en2m_get_pairing(void);
void     en2m_set_pairing(bool enabled);
void     en2m_set_name(const char *name);       /* 运行期改 MQTT slug */

bool en2m_lookup_route(const uint8_t dest[6], uint8_t next_hop[6]);
void en2m_forget_route(const uint8_t dest[6]);
void en2m_clear_routes(void);
```

路由那三个函数**没有内部加锁**，只适合在 en2m 上下文
（比如 `on_uplink` 回调里）调用。详见
[concurrency.md](concurrency.md#6-每个公开-api-的可调用上下文)。

---

## 相关文档

- 入网状态机和 ACK 时间线 → [state-flow.md](state-flow.md)
- 全部超时的调参 → [kconfig.md](kconfig.md)
- USB / MQTT 协议 → [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md)
- 设备不上线的排查 → [troubleshooting.md](troubleshooting.md)

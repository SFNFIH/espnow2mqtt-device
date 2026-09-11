# 配置项与调参

`en2m` 的编译期配置全部走 ESP-IDF 标准的 Kconfig，菜单在

```
idf.py menuconfig  →  Component config  →  ESP-NOW Mesh (en2m)
```

也可以直接写进 `sdkconfig.defaults`（推荐，可进版本控制）：

```
CONFIG_EN2M_WIFI_CHANNEL=6
CONFIG_EN2M_QUEUE_LEN=12
CONFIG_EN2M_MAX_ENDPOINTS=2
```

运行期配置（角色、名字、上报间隔、任务栈）**不在这里**，在
`en2m_device_config_t` 里，见 [api-reference.md](api-reference.md#en2m_device_config_t)。

> 所有 Kconfig 项在 `en2m_mesh.h` / `en2m_model.h` 里都有 `#ifdef CONFIG_… #else …`
> 兜底，所以即使把组件拷进一个没有跑 Kconfig 的工程里也能编译，用的是内建默认值
> （和菜单默认值一致）。

---

## 1. 全部选项一览

| 选项 | 默认 | 范围 | 一句话 | 必须全网一致？ |
|---|---|---|---|---|
| `CONFIG_EN2M_WIFI_CHANNEL` | 1 | 1–14 | ESP-NOW 用的 Wi-Fi 信道 | **是** |
| `CONFIG_EN2M_HOP_LIMIT` | 8 | 1–16 | 最大跳数，同时是 cost 上限 | 建议一致 |
| `CONFIG_EN2M_MAX_ROUTES` | 32 | 4–64 | 反向路由表条数 | 否（只协调器/路由器用得上） |
| `CONFIG_EN2M_MAX_NEIGHBORS` | 16 | 4–32 | 邻居表条数 | 否 |
| `CONFIG_EN2M_BEACON_INTERVAL_MS` | 5000 | 500–60000 | 协调器/路由器发 beacon 的周期 | 否，但影响全网 |
| `CONFIG_EN2M_PARENT_STALE_MS` | 20000 | 5000–120000 | 多久收不到父节点就判定失联 | 否 |
| `CONFIG_EN2M_ROUTE_STALE_MS` | 120000 | 10000–600000 | 路由条目多久不用就作废 | 否 |
| `CONFIG_EN2M_OFFLINE_MS` | 90000 | 10000–600000 | 协调器判定设备离线的阈值 | 否（**只协调器用**） |
| `CONFIG_EN2M_HEARTBEAT_MS` | 30000 | 5000–600000 | 叶子/路由器发心跳的周期 | 否 |
| `CONFIG_EN2M_QUEUE_LEN` | 8 | 4–32 | `en2m` 任务统一队列的深度 | 否 |
| `CONFIG_EN2M_MAX_PENDING` | 4 | 1–16 | 同时等 ACK 的下行数 | 否（**只协调器用**） |
| `CONFIG_EN2M_CMD_RETRIES` | 3 | 0–10 | 下行重传次数 | 否（只协调器用） |
| `CONFIG_EN2M_CMD_RETRY_MS` | 400 | 50–5000 | 重传间隔 | 否（只协调器用） |
| `CONFIG_EN2M_MAX_ENDPOINTS` | 4 | 1–16 | 每设备 endpoint 数 | 否 |
| `CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT` | 8 | 1–16 | 每 endpoint cluster 数 | 否 |
| `CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER` | 6 | 1–16 | 每 cluster attribute 数 | 否 |

"必须全网一致"那一列很重要：**只有信道是硬要求**。其它都是本节点自己的资源和时序，
改了不会让别人收不到你。

---

## 2. 信道

```
CONFIG_EN2M_WIFI_CHANNEL=1
```

ESP-NOW 不跨信道。协调器、路由器、叶子**必须**配同一个值，否则设备永远看不到
beacon，日志里既没有 `parent=`，也不会报错——它就是安静地找不到父节点。

节点不会去连家里的 AP（STA 模式但不 join），所以这个信道是你独占的。但物理上它
和家里的 Wi-Fi 共享频谱，选择策略：

| 家里 AP 在 | en2m 选 | 说明 |
|---|---|---|
| 信道 1 | 6 或 11 | 错开，互不干扰 |
| 信道 6 | 1 或 11 | 同上 |
| 信道 11 | 1 或 6 | 同上 |
| 自动跳信道 | 1 / 6 / 11 里选一个，然后把 AP 锁死 | AP 跳到你头上时会明显丢包 |
| 5 GHz only | 随便，1 最省事 | 2.4 GHz 完全空着 |

> 改信道要**同时重刷协调器和所有设备**。只刷一半会让那一半集体掉线。
> 这也是为什么建议把它写进 `sdkconfig.defaults` 而不是靠 menuconfig 手点。

---

## 3. 拓扑与跳数

### `CONFIG_EN2M_HOP_LIMIT`（默认 8）

两个作用：

1. 写进每个包的 `hop_limit` 字段，转发时 `hop` 超过它就丢包——防环。
2. 选父时 `new_cost > EN2M_HOP_LIMIT` 的候选直接拒绝——防止挂到一条无限长的链上。

| 部署 | 建议 |
|---|---|
| 一个协调器 + 一圈叶子（最常见） | 保持 8，或者压到 2 省一点判断 |
| 加了 1–2 层路由器 | 8 够用 |
| 大平层 / 多层楼，3 层以上路由器 | 8 仍然够；16 是上限，但跳数越多延迟和丢包越难看 |

### `CONFIG_EN2M_MAX_NEIGHBORS`（默认 16）

邻居表记的是"我听见过谁、它的角色/cost/RSSI/最后出现时间"。**只影响选父质量**：
表满了之后新邻居会挤掉最旧的，如果恰好挤掉了那个最好的父节点候选，选父会变差。

| 周围（一跳内）能听见的 en2m 节点数 | 建议 |
|---|---|
| < 10 | 8 就够，省 192 字节 |
| 10–16 | 默认 16 |
| > 16（节点很密） | 24 或 32 |

每条 24 字节，所以 16 → 32 只多 384 字节。

### `CONFIG_EN2M_MAX_ROUTES`（默认 32）

反向路由表：协调器/路由器靠它知道"要发给 X，下一跳是谁"。

**叶子节点用不到路由表**（它只往父节点发），所以纯叶子固件可以压到最小值 4，
省 672 字节。协调器和路由器要按"经过我的设备数"来配：

| 经过这个节点的设备数 | 建议 |
|---|---|
| 叶子（不转发） | 4（最小值） |
| 小网（≤ 20 个设备） | 默认 32 |
| 中网（20–60） | 64（上限） |
| > 64 | 加一层路由器分流，而不是继续加表 |

表满时新路由会挤掉最旧的；被挤掉的设备下一次上行会重新教会协调器路由，
所以后果是"偶尔多一次往返"，不是"控制不了"。

---

## 4. 时序

五个超时是有层级关系的，随便改一个很容易把层级打乱。默认值的关系是：

```
beacon 5s ──┬── parent_stale 20s  （= 4 个 beacon 周期）
            │
heartbeat 30s ──── offline 90s     （= 3 个心跳周期）
                        │
                  route_stale 120s
```

**调参的第一原则：保持倍数关系，不要只改一个。**

### `CONFIG_EN2M_BEACON_INTERVAL_MS`（默认 5000）

协调器和路由器广播 beacon 的周期。叶子靠 beacon 发现父节点，也靠它判断父节点还活着。

| 改动 | 好处 | 代价 |
|---|---|---|
| 调小（2000） | 入网更快、父节点切换更快 | 空口占用变高；周围节点被唤醒更频繁 |
| 调大（15000） | 空口更干净 | 冷启动入网慢（最坏要等一个周期）；父节点挂了要更久才发现 |

**调它必须同时调 `PARENT_STALE_MS`**，见下。

### `CONFIG_EN2M_PARENT_STALE_MS`（默认 20000）

叶子/路由器多久没听到父节点的 beacon 就宣布 `parent stale`，清掉父节点、
发 `EN2M_EVENT_PARENT_LOST`、重新找。

规则：**`PARENT_STALE_MS ≈ 3~4 × BEACON_INTERVAL_MS`**。

小于 2 倍会因为偶尔丢一个 beacon 就误判掉线（然后立刻又找回来，日志里一串
`parent stale` / `parent=`）；大于 6 倍会让真的换了位置的设备很久缓不过来。

| beacon | 建议 parent_stale |
|---|---|
| 2000 | 8000 |
| 5000（默认） | 20000（默认） |
| 10000 | 35000 |

> 注意：父节点的"活着"判断只靠 **beacon**，不靠数据包。你的设备每秒都在成功
> 上报，但如果 beacon 全丢了，它仍然会到点宣布 `parent stale`。所以不要把
> `BEACON_INTERVAL_MS` 调得比 `HEARTBEAT_MS` 还大。

### `CONFIG_EN2M_HEARTBEAT_MS`（默认 30000）

叶子/路由器多久发一次心跳（`EN2M_MSG_HEARTBEAT`），让协调器知道自己还在。
心跳和上报是两件事：心跳是**传输层**的存活证明，上报是数据。

实现是纯定时的：`en2m_maintenance` 每 100 ms 检查一次
`now - last_hello >= EN2M_HEARTBEAT_MS`，到点就发，**不看这期间有没有发过状态上报**。
所以一个 10 秒上报一次的设备仍然会每 30 秒额外发一个心跳帧。它最要紧的场合是
`EN2M_REPORT_ON_CHANGE_ONLY` 的设备——好几天不变化，靠心跳撑住在线状态。

| 设备 | 建议 |
|---|---|
| 常电、上报本来就频繁 | 调大到 90000，让上报自己承担存活证明，省掉多余的帧 |
| 常电、`ON_CHANGE_ONLY` | 默认 30000 |
| 电池设备 | 调大（120000+），每次发包都是电 |
| 要求快速感知掉线 | 15000，同时把协调器的 `OFFLINE_MS` 调到 45000 |

### `CONFIG_EN2M_OFFLINE_MS`（默认 90000）

**这个值只有 S3 协调器固件会读**（`firmware/coordinator`：peer 的 `last_ms`
超过它就往主机发一条 `offline` 事件，HA 里的实体变成不可用）。设备侧编译进去
不起任何作用。

规则：**`OFFLINE_MS ≥ 3 × 全网最大的 HEARTBEAT_MS`**。只留 2 倍余量的话，
一次心跳丢包就会让 HA 里的实体闪一下"不可用"。

### `CONFIG_EN2M_ROUTE_STALE_MS`（默认 120000）

路由条目多久不刷新就作废。

这个值有一个**实际会咬人的副作用**：设备重启换了父节点之后，协调器手里的旧路由
最多还要 120 秒才过期，在这之前下行会往错误的下一跳发，命令会 ACK 超时。
缓解办法有两个：

- 调小到 30000–60000（代价：路由表更容易空，多一次上行往返去重建）
- 让设备重启后立刻主动上行一次（`en2m_report_now`，默认配置本来就会在启动
  500 ms 后报一次），协调器收到就会刷新路由

细节见 [mesh.md](mesh.md#配网与重连)。

---

## 5. 队列

### `CONFIG_EN2M_QUEUE_LEN`（默认 8）

`en2m` 任务只有**一个**队列，三种东西共用它：收到的帧（`EN2M_ITEM_RX`）、
`en2m_schedule` 的工作项（`EN2M_ITEM_WORK`）、ISR 里提交的属性
（`EN2M_ITEM_ATTR`）。共用一个队列是为了保证顺序（详见
[architecture.md](architecture.md#一个队列)）。

**每个槽位 240 字节**（联合体里最大的是一整个 221 字节的帧），所以
8 → 16 要多 1920 字节 DRAM。

队列满时的行为按来源不同：

| 来源 | 队列满时 |
|---|---|
| ESP-NOW 接收回调 | 立刻丢帧（`timeout = 0`，绝不阻塞回调），计数进 `rx_dropped`，攒够了发 `EN2M_EVENT_RX_DROPPED` |
| `en2m_schedule` | 返回 `ESP_ERR_TIMEOUT` / `ESP_FAIL`，工作项丢失 |
| `en2m_schedule_from_isr` | 同上，ISR 里不阻塞 |

什么时候要调大：

| 症状 | 动作 |
|---|---|
| 日志里 `dropped N frames, the queue could not keep up` | 调大到 16 |
| 收到 `EN2M_EVENT_RX_DROPPED` | 同上 |
| 读/写回调里做慢活（阻塞 I2C、100 ms 以上） | 调大，或者把慢活挪到自己的任务 |
| 路由器节点，转发量大 | 16 或 24 |
| 纯叶子、只有一个 cluster | 默认 8 甚至 4 都够 |

> 队列溢出的**根因**通常不是队列太小，而是 `en2m` 任务上有人占着不放：
> 一个读回调里 `vTaskDelay(500)`，或者一个 `en2m_schedule` 的工作项在做 flash 擦除。
> 先查这个，再调队列。

---

## 6. 下行可靠性（只影响协调器）

三个选项决定"HA 点一下开关，最多花多久确认失败"：

| 选项 | 默认 |
|---|---|
| `CONFIG_EN2M_MAX_PENDING` | 4 |
| `CONFIG_EN2M_CMD_RETRIES` | 3 |
| `CONFIG_EN2M_CMD_RETRY_MS` | 400 |

默认值下，一条命令的时间线是：

| 时刻 | 发生 |
|---|---|
| 0 ms | 第 1 次发送 |
| 400 ms | 第 2 次（重传 1） |
| 800 ms | 第 3 次（重传 2） |
| 1200 ms | 第 4 次（重传 3，最后一次） |
| 1600 ms | 仍无 ACK → `EN2M_EVENT_ACK_TIMEOUT`，槽位释放 |

即 **`(retries + 1) × retry_ms` ≈ 1.6 秒**得到结论。

| 想要 | 怎么改 |
|---|---|
| 更快知道失败（HA 里少转圈） | `RETRIES=2`、`RETRY_MS=250` → 0.75 s |
| 多跳网络，容忍更差的链路 | `RETRIES=5`、`RETRY_MS=600` → 3.6 s |
| 完全不要重传（自己在上层处理） | `RETRIES=0`，或者发下行时传 `cmd_id = 0`（那样连 ACK 都不等） |

`MAX_PENDING` 是**同时**在等 ACK 的命令数。满了之后新命令会照常发出去，
但不进重传表，日志里是
`no free retry slot; command N is sent unacknowledged`。
HA 里一键关所有灯（十几条命令挤在一起）就会撞到这个，把它调到 8 或 16。
每个槽位 248 字节。

---

## 7. 数据模型容量

三个乘在一起决定静态 RAM 占用：

```
endpoints × clusters_per_endpoint × attributes_per_cluster
```

实测（RISC-V 32 位，`riscv32-esp-elf-gcc`）：

| 结构 | 字节 |
|---|---|
| 一个 attribute 槽位 | 24 |
| 一个 cluster（含 6 个 attribute 槽 + 6 个回调指针） | 176 |
| 一个 endpoint（含 8 个 cluster 槽） | 1416 |
| **默认 4 个 endpoint 的整张表** | **5664** |

注意这是**静态数组，全部预分配**，不管你实际建了几个 endpoint 都占这么多。
所以调小这三个值是设备固件里最划算的 RAM 优化：

| 配置 | endpoint 大小 | 整张表 | 适用 |
|---|---|---|---|
| 4 × 8 × 6（默认） | 1416 | 5664 | 通用，随便加东西 |
| 1 × 4 × 4 | 4 × (4+4×24) ≈ 408 | **408** | 单一功能的开关/传感器 |
| 1 × 2 × 2 | ≈ 160 | **160** | 极简继电器 |
| 2 × 8 × 6 | 1416 | 2832 | 二合一设备 |

对照各设备类型实际需要多少：

| 设备 | endpoint | cluster | 最多 attribute/cluster |
|---|---|---|---|
| 继电器 / 插座 | 1 | 1（OnOff） | 1 |
| 调光灯 | 1 | 2（OnOff + Level） | 1 |
| 色温灯 | 1 | 3 | 1 |
| 温湿度 | 1 | 2 | 1 |
| 智能插座（带计量） | 1 | 2（OnOff + ElectricalPower） | 2 |
| 风扇 | 1 | 1（FanControl） | 2 |
| 窗帘 | 1 | 1 | 1 |
| 门锁 | 1 | 1 | 1 |
| **温控器** | 1 | 1（Thermostat） | **4** ← 最多的 |
| 烟感 | 1 | 1（SmokeCO） | 2 |
| 人体 + 光照 | 1 | 2 | 1 |

设备类型配方**不会**自动加 `Identify` cluster，想要的话自己
`en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY)`（它只有 1 个属性）。
留出这一格之后，**`1 × 4 × 4` 能装下上表里任何一种单功能设备**，比默认省 5.2 KB。

超容量时的行为是**建的时候就失败并打日志**，不会静默截断：

| 日志 | 原因 |
|---|---|
| `no free endpoint slot (EN2M_MAX_ENDPOINTS=4)` | endpoint 用满 |
| `endpoint N is full, cannot add cluster 0xXXXX` | 这个 endpoint 的 cluster 用满 |
| `cluster 0xXXXX is full, cannot add attribute 0xXXXX` | 这个 cluster 的 attribute 用满 |

所以只要开机日志干净，容量就是够的。

---

## 8. 不在 Kconfig 里、但同样是配置的东西

| 东西 | 在哪 | 默认 |
|---|---|---|
| 角色（coordinator/router/leaf） | `cfg.mesh.role` | 无默认，必填 |
| 设备名、型号 | `cfg.mesh.name` / `.model` | `"node1"` / `"c3-node"` |
| 信道覆盖（运行期） | `cfg.mesh.channel` | 0 = 用 `CONFIG_EN2M_WIFI_CHANNEL` |
| 重传次数/间隔覆盖 | `cfg.mesh.max_retries` / `.retry_interval_ms` | 0 = 用 Kconfig 值 |
| 上报模式 | `cfg.report_mode` | `EN2M_REPORT_DEFAULT` |
| 周期上报间隔 | `cfg.report_interval_ms` | 0 = 按角色（LEAF 30000，常电 15000） |
| 上报限流下限 | `cfg.min_report_interval_ms` | 0 = 1000 |
| `en2m` 任务栈 | `cfg.task_stack_size` | 0 = 4096 |
| `en2m` 任务优先级 | `cfg.task_priority` | 0 = 5 |
| 维护节拍 | `EN2M_TICK_MS`（`en2m_priv.h`，非公开） | 100 |
| 上报载荷上限 | `EN2M_DATA_MAX`（`en2m_proto.h`） | 160 |
| NVS 命名空间 | `"en2m_attr"`（`en2m_datamodel.c`） | 固定 |

运行期覆盖优先于 Kconfig：`cfg.mesh.channel = 6` 会盖掉
`CONFIG_EN2M_WIFI_CHANNEL=1`。这对"同一份固件刷不同信道"有用，
但记住信道必须全网一致。

### 任务栈怎么定

默认 4096 字节要同时装下：ESP-NOW 收包处理、JSON 解析（命令下行）、
上报组包、**以及你的读/写/命令/变化四个回调**。

| 情况 | 建议 |
|---|---|
| 回调里只写 GPIO | 默认 4096 |
| 回调里做 I2C / SPI、带驱动库 | 6144 |
| 回调里 `snprintf` 大缓冲、或者递归 | 8192 |
| 回调里碰文件系统 / TLS | 别放回调里，用 `en2m_schedule` 也不行——自己建任务 |

栈溢出的表现是 `***ERROR*** A stack overflow in task en2m has been detected`。
看实际余量：

```c
ESP_LOGI(TAG, "en2m stack high water: %u", uxTaskGetStackHighWaterMark(NULL));
```
（在一个 `en2m_schedule` 的工作项里调，那时 `NULL` 就是 `en2m` 任务。）

### 优先级怎么定

默认 5。它需要比你的慢速应用任务高（不然收包会被拖），比 Wi-Fi 栈
（ESP-IDF 里通常 18–23）低。**不要**调到 18 以上，会和 Wi-Fi 抢 CPU 并开始丢包。

---

## 9. 三套推荐配置

直接拷进 `sdkconfig.defaults`。

### 极简电池叶子（最省 RAM）

```
CONFIG_IDF_TARGET="esp32c3"
CONFIG_FREERTOS_HZ=1000

CONFIG_EN2M_WIFI_CHANNEL=6
CONFIG_EN2M_MAX_ENDPOINTS=1
CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT=4
CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER=4
CONFIG_EN2M_MAX_ROUTES=4
CONFIG_EN2M_MAX_NEIGHBORS=8
CONFIG_EN2M_QUEUE_LEN=4
CONFIG_EN2M_MAX_PENDING=1
CONFIG_EN2M_HEARTBEAT_MS=120000
```

比默认省下大约 `5256 (模型) + 672 (路由) + 192 (邻居) + 960 (队列) + 744 (重传)`
≈ **7.8 KB** DRAM。

### 通用常电设备（默认就好）

```
CONFIG_IDF_TARGET="esp32c3"
CONFIG_FREERTOS_HZ=1000
CONFIG_EN2M_WIFI_CHANNEL=6
```

### 路由器节点（转发量大）

```
CONFIG_IDF_TARGET="esp32c3"
CONFIG_FREERTOS_HZ=1000

CONFIG_EN2M_WIFI_CHANNEL=6
CONFIG_EN2M_MAX_ROUTES=64
CONFIG_EN2M_MAX_NEIGHBORS=24
CONFIG_EN2M_QUEUE_LEN=24
CONFIG_EN2M_MAX_ENDPOINTS=1
CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT=2
CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER=2
```

路由器不需要数据模型（`firmware/router` 连 `en2m_start` 都不调，
直接 `en2m_mesh_init`），所以三个容量全压到最小。

---

## 10. 改完怎么验证

```bash
idf.py fullclean            # 容量宏是编译期的，改了一定要 fullclean
idf.py build
idf.py size                 # 看 DRAM 有没有降下来
idf.py size-components      # 看 en2m 自己占多少
```

`idf.py size` 里看 `.bss` 的变化——数据模型和 mesh 状态都是静态数组，
全在 `.bss` 里。

改完之后应该确认的三件事：

1. 开机日志里**没有** `no free … slot` / `is full` 之类的行
2. `parent=` 那行按预期出现，而且之后不再反复出现 `parent stale`
3. 跑一小时，没有 `dropped N frames` 和 `command N to … was never acknowledged`

排错见 [troubleshooting.md](troubleshooting.md)。

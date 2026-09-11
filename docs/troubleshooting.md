# 按症状排错

先看 [第 0 节](#0-三十秒定位)定位到大致方向，再跳到对应小节。

- [0. 三十秒定位](#0-三十秒定位)
- [1. 设备完全不上线](#1-设备完全不上线)
- [2. 设备上线又掉线](#2-设备上线又掉线)
- [3. HA 里没有实体](#3-ha-里没有实体)
- [4. HA 里有实体但状态不更新](#4-ha-里有实体但状态不更新)
- [5. 控制不生效 / 状态弹回](#5-控制不生效--状态弹回)
- [6. 状态更新太慢 / 太频繁](#6-状态更新太慢--太频繁)
- [7. 重启后状态丢了](#7-重启后状态丢了)
- [8. 崩溃、看门狗、栈溢出](#8-崩溃看门狗栈溢出)
- [9. 编译和链接错误](#9-编译和链接错误)
- [10. 组件全部日志的含义](#10-组件全部日志的含义)

---

## 0. 三十秒定位

接上串口（`idf.py -p /dev/ttyACM0 monitor`），看开机后 10 秒内有没有这两行：

```
I (612)  en2m: mesh init
I (1843) en2m: parent=aa:bb:cc:dd:ee:ff cost=1 rssi=-42
```

| 看到什么 | 去哪 |
|---|---|
| 两行都没有 | [第 9 节](#9-编译和链接错误)，或者固件根本没跑起来 |
| 只有 `mesh init`，没有 `parent=` | [第 1 节](#1-设备完全不上线) |
| 有 `parent=`，之后反复出现 `parent stale` | [第 2 节](#2-设备上线又掉线) |
| 两行都正常，但 HA 里啥也没有 | [第 3 节](#3-ha-里没有实体) |
| HA 里有实体，状态不动 | [第 4 节](#4-ha-里有实体但状态不更新) |
| HA 里能看不能控 | [第 5 节](#5-控制不生效--状态弹回) |
| 一直在 `Guru Meditation` / 重启 | [第 8 节](#8-崩溃看门狗栈溢出) |

想不靠串口判断，把 12 个事件接上，`EN2M_EVENT_PARENT_FOUND` /
`PARENT_LOST` / `RX_DROPPED` 三个就覆盖了大部分故障。
代码见 [events.md](events.md#完整的诊断处理器)。

---

## 1. 设备完全不上线

症状：有 `mesh init`，**永远没有** `parent=`。

按可能性从高到低查：

### 1.1 信道不一致（最常见）

ESP-NOW 不跨信道。设备和协调器必须同一个信道，**不一致时完全静默**，
既不报错也不打日志。

```bash
# 设备侧
grep EN2M_WIFI_CHANNEL sdkconfig
# 协调器侧（espnow2mqtt-host 仓库）
grep EN2M_WIFI_CHANNEL firmware/coordinator/sdkconfig
```

两个必须相同。还要检查运行期有没有被覆盖：`cfg.mesh.channel` 非 0 时会盖掉
Kconfig 的值。

```c
en2m_device_config_t cfg = {
    .mesh = {.role = ..., .channel = 0},   /* 0 = 用 CONFIG_EN2M_WIFI_CHANNEL */
};
```

见 [kconfig.md](kconfig.md#2-信道)。

### 1.2 协调器没在发 beacon

叶子靠 beacon 发现父节点。协调器那边确认：

- S3 已经插上，`firmware/coordinator` 跑起来了
- 它的角色是 `EN2M_ROLE_COORDINATOR`（不是 LEAF——LEAF 不发 beacon）
- 串口上能看到它在收发

只有 `COORDINATOR` 和 `ROUTER` 发 beacon。如果你把中继节点配成了
`EN2M_ROLE_LEAF`，它下游的设备永远找不到父节点。

### 1.3 距离太远 / RSSI 太低

`parent=` 那行会带 `rssi=`。完全没有这行说明一个 beacon 都没收到。

- 把设备搬到协调器旁边 30 cm，重启，看能不能出 `parent=`
- 能出 → 是距离问题，加一个 `EN2M_ROLE_ROUTER` 中继（见
  [examples.md](examples.md#firmwarerouter)）
- 还是不能 → 回到 1.1 和 1.2

### 1.4 `hop_limit` 太小

多跳部署时，`new_cost > EN2M_HOP_LIMIT` 的候选会被拒。默认 8 足够大，
但如果你把它压到 1，那么只有直连协调器的节点能入网。

### 1.5 Wi-Fi 被别的代码抢了

组件在 `en2m_mesh_init` 里自己 `esp_wifi_init` + `esp_wifi_set_mode(WIFI_MODE_STA)`
并且**不 join AP**。如果你的应用也初始化了 Wi-Fi、或者连了家里的路由器，
信道会被 AP 拽走，ESP-NOW 就收不到东西了。

**同一个固件不能既连 AP 又跑 en2m**（除非 AP 恰好在同一个信道）。

### 1.6 天线 / 硬件

- 模块天线没焊 / 屏蔽罩压到天线
- 供电不足：ESP32-C3 发射瞬间要 350 mA 峰值，USB 供电不够会静默复位
  （日志里会看到反复的启动 banner）

---

## 2. 设备上线又掉线

症状：`parent=` 和 `parent stale` 交替出现。

### 2.1 `PARENT_STALE_MS` 相对 beacon 太短

默认 `beacon = 5 s`、`parent_stale = 20 s`，即容忍丢 3 个 beacon。

如果你调过这两个值，**保持 `PARENT_STALE_MS ≈ 3~4 × BEACON_INTERVAL_MS`**。
小于 2 倍就会因为偶尔丢一个 beacon 而误判掉线，然后立刻又找回来——
日志里就是一串交替。见 [kconfig.md](kconfig.md#4-时序)。

### 2.2 只有 beacon 决定"父节点还活着"

这是一个容易被忽略的实现细节：父节点的存活判断**只看 beacon**，
不看数据帧。你的设备每秒都在成功上报，但如果 beacon 全丢了，
它仍然会到点宣布 `parent stale`。

所以不要把 `BEACON_INTERVAL_MS` 调得很大（比如 30 s）而
`PARENT_STALE_MS` 不动——那必然反复掉线。

### 2.3 边界位置来回换父

装了路由器之后，处在两个父节点之间的设备会来回切。组件里有 **8 dB 的选父滞回**
（新候选必须比当前父节点好 8 dB 以上才切换）来压制这个，但如果两个父节点的
RSSI 本身在剧烈抖动（有人走动、有金属门开合），还是会切。

缓解：把设备或路由器挪一下，让某一个明显更强。
选父算法见 [mesh.md](mesh.md#5-beacon-与选父)。

### 2.4 供电抖动

反复出现完整的 ESP-IDF 启动 banner（而不只是 `parent stale`）说明是**复位**
而不是掉线。查供电和 [第 8 节](#8-崩溃看门狗栈溢出)。

### 2.5 协调器侧判定的离线

HA 里显示"不可用"但设备串口上一切正常 → 是协调器的 `EN2M_OFFLINE_MS`
（默认 90 s）到了。

规则：**`OFFLINE_MS ≥ 3 × HEARTBEAT_MS`**。默认是 90 / 30，刚好 3 倍。
如果你把设备的 `HEARTBEAT_MS` 调大到 120 s 却没动协调器的 `OFFLINE_MS`，
设备会每隔 90 秒被判一次离线。

---

## 3. HA 里没有实体

`parent=` 正常但 HA 里什么都没有。往上游一层一层查。

### 3.1 设备真的发出了上报吗

监听 `EN2M_EVENT_REPORT_SENT`，或者看有没有这条 ERROR：

```
E en2m_model: report exceeds 160 bytes; split the device across endpoints or trim clusters
```

出这条说明连三级降级都没救回来，报文没发出去。解决：减少 cluster，
或者缩短 `cfg.mesh.name`（名字占 16 字节）。见
[reporting.md](reporting.md#160-字节降级)。

### 3.2 endpoint 建失败了

```
E en2m_dm: invalid endpoint id 0                       ← id 必须 1..254
E en2m_dm: endpoint 1 already exists
E en2m_dm: no free endpoint slot (EN2M_MAX_ENDPOINTS=4)
E en2m_dm: endpoint 1 is full, cannot add cluster 0x0006
E en2m_dm: cluster 0x0006 is full, cannot add attribute 0x0000
```

开机日志里有这些行的话，模型压根没建起来。前两条是代码 bug，
后三条是容量不够——调 Kconfig，见 [kconfig.md](kconfig.md#7-数据模型容量)。

**特别注意**：`en2m_endpoint_create_device` 返回 `NULL` 时如果你没检查，
后面 `en2m_start` 会正常启动一个**空模型**，上报里除了 `caps` 什么都没有，
HA 那边就生成不出实体。所以一定要：

```c
if (en2m_endpoint_create_device(ENDPOINT, TYPE) == NULL) {
    ESP_LOGE(TAG, "could not create the endpoint");
    return;
}
```

### 3.3 `caps` 没带上

HA 集成靠上报里的 `caps` 数组决定生成哪些实体。三级降级里
`caps` 是**最后**才被砍的，但一旦被砍，第一次上报就不会带它。

如果设备刚上线时报文正好超长，HA 可能就错过了那一次。等下一次上报
（默认 30 s）通常就好了。一直不行看 3.1。

### 3.4 bridge / MQTT 那一层

设备侧都正常的话，问题在主机侧。到
[espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host) 排：

```bash
mosquitto_sub -t 'espnow2mqtt/#' -v          # 有没有消息
mosquitto_sub -t 'homeassistant/#' -v        # 有没有 discovery
```

| 现象 | 方向 |
|---|---|
`espnow2mqtt/bridge/state` 是 `offline` | bridge 没跑，或者 MQTT 连不上 |
| 有 `espnow2mqtt/<mac>/state` 但没有 `homeassistant/...` discovery | bridge 的 discovery 没发，检查 `caps` 和 bridge 日志 |
| discovery 也有但 HA 里没实体 | HA 的 MQTT 集成没启用 discovery，或者 discovery 前缀不匹配 |
| 什么都没有 | 协调器的 USB 没通，看 bridge 日志有没有在读串口 |

---

## 4. HA 里有实体但状态不更新

### 4.1 值没变就不上报（这是正常的）

组件在属性存储层**去重**：`en2m_attribute_set` 写入的值和当前值相等时，
什么都不会发生——不上报、不触发 `attribute_changed`、不写 NVS。

所以"我每秒调 `en2m_report_temperature`，HA 里只偶尔更新"是**设计如此**。
真的要每次都发：

```c
en2m_report_now();      /* 注意：只在 en2m 任务上才是"立即发" */
```

见 [reporting.md](reporting.md#去重与限流的区别)。

### 4.2 读回调返回了 `ESP_ERR_NOT_SUPPORTED`

这个返回值的意思是"这个属性不是我管的，保留缓存值"。写错了（比如
`switch` 里漏了一个 case）就会导致那个属性永远是默认值。

```c
default:
    return ESP_ERR_NOT_SUPPORTED;   /* ← 检查是不是漏了你的 cluster */
```

排查办法：接上 `attribute_changed` 回调打日志，看到底哪些属性在动。

### 4.3 读回调根本没被注册

```c
en2m_device_config_t cfg = {
    .mesh = {...},
    .attribute_read = on_read,      /* ← 漏了这行的话回调永远不被调 */
};
```

或者用了 `en2m_cluster_set_read_cb` 但传的 `en2m_cluster_t *` 是 `NULL`
（`en2m_cluster_get` 找不到时返回 `NULL`，而 `en2m_cluster_set_read_cb(NULL, ...)`
会返回 `ESP_ERR_INVALID_ARG`——所以一定要 `ESP_ERROR_CHECK` 它）。

### 4.4 属性 ID 撞车

九个不同 cluster 的主属性 ID 都是 `0x0000`。回调里只
`switch (path->attribute_id)` 会张冠李戴：温度的读回调会被湿度的请求命中。

**永远先 `switch (path->cluster_id)`。** 撞车清单见
[data-model.md](data-model.md#属性-id-会撞车)。

### 4.5 上报被 `min_report_interval_ms` 限流

默认 1000 ms。变化比这更快的属性会被合并——最后一次的值一定会发出去，
中间的过程会被跳过。这对滑动亮度条是好事，对"我要看每一次脉冲"是坏事。

想更快就调小（50、100）；但想清楚空口负载。见
[reporting.md](reporting.md#min_report_interval_ms)。

### 4.6 上报模式选错了

| 模式 | 表现 |
|---|---|
| `EN2M_REPORT_DEFAULT` | 变化即报（限流）+ 周期保活 |
| `EN2M_REPORT_PERIODIC_ONLY` | **变化不报**，只按周期。看着就像"状态很迟钝" |
| `EN2M_REPORT_ON_CHANGE_ONLY` | 不变就永远不报。HA 重启后要等第一次变化才有值 |
| `EN2M_REPORT_MANUAL` | **组件完全不主动上报**，只有你调 `en2m_report_now` 才发 |

`.report_mode` 没显式设时是 `EN2M_REPORT_DEFAULT`（枚举值 0），这通常是对的。

### 4.7 多 endpoint 只有一个生效

当前实现的属性存储用**全局扁平键**，上报时同一个 cluster 只取**编号最小**的
endpoint。所以"endpoint 1 和 2 各挂一个 OnOff"在 HA 里只会出现一个开关。

多外设请用**多 cluster**（`en2m_cluster_set_write_cb`），不要用多 endpoint。
见 [data-model.md](data-model.md#多-endpoint-的限制)。

---

## 5. 控制不生效 / 状态弹回

### 5.1 状态在 HA 里弹回去了

这是**写回调返回了错误**的标准表现：组件拒绝提交，所以下一次上报里
还是旧值，HA 就把开关拨回去。

```
W en2m_dm: write 1/0x0006/0x0000 rejected: ESP_FAIL
```

两种原因：

| 原因 | 修 |
|---|---|
| 你的硬件操作真的失败了 | 修硬件/驱动 |
| 你在**不认识的路径**上返回了 `ESP_FAIL` | 改成 `ESP_ERR_NOT_SUPPORTED` |

第二种是最常见的迁移错误。`ESP_ERR_NOT_SUPPORTED` = "不是我管的，往下走"；
`ESP_FAIL` = "是我管的，但我失败了"。见
[callbacks.md](callbacks.md#6-三级-fall-through)。

### 5.2 完全没反应，日志里也没动静

```
W en2m_model: command payload is not valid JSON
W en2m_model: command payload had nothing this device understands
W en2m_model: command <name>: no endpoint exposes cluster 0x0102
W en2m_model: unhandled command '<name>' on cluster 0x0006
```

| 日志 | 意思 |
|---|---|
| `not valid JSON` | 下行载荷坏了或者被 160 字节截断。这种情况**不会回 ACK**，协调器会重传 4 次然后报超时 |
| `nothing this device understands` | JSON 合法但没有一个键能解析成命令。查协调器/bridge 的命令格式 |
| `no endpoint exposes cluster 0x…` | 设备上没有这个 cluster。HA 发了个你没实现的功能 |
| `unhandled command` | 命令认出来了但没人处理：你的 `command` 回调返回了 `NOT_SUPPORTED`，内置翻译也不支持 |

一条 WARN 都没有说明**帧根本没到设备**，看 5.4。

### 5.3 命令 ACK 超时

协调器侧日志：

```
W en2m: command 123 to aa:bb:cc:dd:ee:ff was never acknowledged
```

HA 里的表现是点了没反应、可能转一会儿圈。原因：

| 原因 | 修 |
|---|---|
| 设备刚重启换了父节点，协调器手里的路由是旧的 | 等 `EN2M_ROUTE_STALE_MS`（默认 **120 秒**）过期，或者把它调到 30–60 s。这是个真实的坑，见 [mesh.md](mesh.md#配网与重连) |
| 链路太差，4 次都丢了 | 调大 `CONFIG_EN2M_CMD_RETRIES` / `RETRY_MS`，或者加路由器 |
| 设备侧 JSON 解析失败（见 5.2） | 修载荷格式 |
| 设备还没 `en2m_start` 完 | 正常，重试会成功 |

`cmd_id = 0` 的下行**不等 ACK**，所以不会有这条日志也不会重传。

### 5.4 `drop uplink (not pairing / unknown)`

协调器侧日志。意思是收到一个**不认识的设备**的上行，而且当前**不在配网窗口**，
所以丢掉了。

修：在协调器上打开配网（`en2m_set_pairing(true)`，HA 集成里通常有个按钮），
让设备被记住一次。见 [mesh.md](mesh.md#7-配网窗口)。

### 5.5 窗帘的"停止"没反应

```
W en2m: stop needs a command handler or a window covering driver
```

`STOP_MOTION` 没法翻译成一次属性写入——只有应用知道电机停在哪。
必须在 `command` 回调里实现它：

```c
case EN2M_CMD_STOP_MOTION:  motor_stop();  return ESP_OK;
```

见 [examples.md](examples.md#window_cover)。

### 5.6 本地按键改了硬件但 HA 不知道

```c
/* 错 */
drv_gpio_relay_set(!on, NULL);

/* 对 */
en2m_attribute_write(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!on));
```

直接调驱动，组件不知道状态变了，也不会上报。让本地控制走
`en2m_attribute_write`，和远程下发同一条路径。

### 5.7 风扇：一条命令回调了两次

这是**正常的**。组件会替你维持 `FAN_MODE` 和 `PERCENT_SETTING` 的一致性：
写完 percent 之后可能再写一次 mode。保证你的写回调是幂等的（把值记下来、
统一算一次输出）就没问题。见 [examples.md](examples.md#fan_controller)。

---

## 6. 状态更新太慢 / 太频繁

一张表对上就行：

| 想改 | 改哪 | 默认 |
|---|---|---|
| 周期上报间隔 | `cfg.report_interval_ms` | LEAF 30000 / 常电 15000 |
| 变化上报的最快频率 | `cfg.min_report_interval_ms` | 1000 |
| 传感器采样频率 | 就是 `report_interval_ms`（读回调在上报前被调） | 同上 |
| 完全自己控制上报时机 | `cfg.report_mode = EN2M_REPORT_MANUAL` + `en2m_report_schedule` | — |
| 心跳频率 | `CONFIG_EN2M_HEARTBEAT_MS` | 30000 |
| 协调器判离线的阈值 | 协调器的 `CONFIG_EN2M_OFFLINE_MS` | 90000 |

三条经验：

1. **上报快 = 耗电多 + 空口忙。** 30 秒对绝大多数传感器都够。
2. **`min_report_interval_ms` 不是"不限流"开关**，填 0 表示取默认 1000。
3. **改采样频率就是改上报频率**，这是"组件持有时序"的直接后果。

详见 [reporting.md](reporting.md#调参)。

---

## 7. 重启后状态丢了

### 7.1 那个属性本来就不持久化

只有一部分属性默认 `persist = true`（执行器状态），传感器读数一律不持久化。
19 个属性的清单见 [persistence.md](persistence.md#哪些属性会持久化)。

自建属性时自己指定：

```c
en2m_attribute_create(cluster, EN2M_ATTR_ON_OFF, en2m_bool(false), true /* persist */);
```

> 目前**没有公开 API 能改已存在属性的 persist 标志**。用配方建的 cluster
> 想改持久化策略，只能先 `en2m_cluster_create` 再用
> `en2m_attribute_create` 覆盖同一个 attribute id。

### 7.2 断电太快，5 秒的批量写盘没赶上

为了省 flash 寿命，脏属性是**每 5 秒批量刷一次**，不是每次变化都写。
所以"改了状态、1 秒内拔电"会丢那次改动。

这是有意的权衡：每次变化都写 NVS 的话，一个来回调亮度的滑动条能在几分钟里
写掉上千次。`en2m_stop()` 会额外刷一次盘，所以有计划的重启不会丢。

### 7.3 恢复时驱动还没初始化

```
W en2m_dm: write 1/0x0006/0x0000 rejected: ESP_ERR_INVALID_STATE
```

开机恢复是**通过 `attribute_write` 回调**做的，所以你的驱动必须在
`en2m_start` 之前 init 好。否则回调失败 → 拒绝提交 → **NVS 里的值被当成
写失败丢掉**，下一次开机又是默认值。

```c
ESP_ERROR_CHECK(drv_gpio_relay_init(PIN_RELAY, true));   /* ← 必须在前面 */
...
ESP_ERROR_CHECK(en2m_start(&cfg));
```

见 [persistence.md](persistence.md#开机恢复)。

### 7.4 NVS 坏了

```
W en2m_dm: nvs_open failed, attribute state will not survive a reboot
```

NVS 分区有问题。`en2m_start` 会自己处理 `NO_FREE_PAGES`（erase 再 init），
但分区表本身缺 `nvs` 分区的话救不了。检查 `partitions.csv`，
或者整片擦掉重来：

```bash
idf.py erase-flash flash
```

只清组件的属性（保留别的 NVS 数据）：命名空间是 `"en2m_attr"`，
用 `nvs_erase_all` 打开那个 namespace 擦。

---

## 8. 崩溃、看门狗、栈溢出

### 8.1 `A stack overflow in task en2m has been detected`

`en2m` 任务默认 4096 字节栈，要同时装下收包处理、JSON 解析、上报组包，
**以及你的四个回调**。回调里做重活就会溢出。

```c
cfg.task_stack_size = 6144;   /* 或 8192 */
```

看实际余量（在一个 `en2m_schedule` 的工作项里调，那时 `NULL` 就是 `en2m` 任务）：

```c
ESP_LOGI(TAG, "en2m stack high water: %u", uxTaskGetStackHighWaterMark(NULL));
```

建议值见 [kconfig.md](kconfig.md#任务栈怎么定)。

### 8.2 在 ISR 里调了不该调的东西

在中断里调 `en2m_attribute_set` / `en2m_attribute_write` / `en2m_report_*` /
`en2m_report_now` / `en2m_schedule`（不带 `_from_isr`）都会去拿互斥锁，
在中断上下文里这是**未定义行为**，通常表现为 `Guru Meditation` 或者莫名死锁。

ISR 里只能用这两个：

```c
en2m_schedule_from_isr(fn, arg, &woken);
en2m_attribute_set_from_isr(ep, cluster, attr, value, &woken);
```

全表见 [concurrency.md](concurrency.md#每个-api-能在哪里调)。

> 陷阱：`esp_timer` 回调**默认不在** ISR 里（跑在 `esp_timer` 任务上），
> 所以可以直接用普通 API。但如果创建 timer 时用了
> `.dispatch_method = ESP_TIMER_ISR`，就真的在中断里了。

### 8.3 `Task watchdog got triggered`

`en2m` 任务被堵住了。原因几乎总是回调里做了长时间阻塞：

| 在回调里 | 后果 |
|---|---|
| `vTaskDelay(500)` | 维护节拍、心跳、重传全部延后，队列开始溢出 |
| 慢速 I2C（>200 ms） | 同上 |
| flash 擦除 | 同上，而且很容易直接触发看门狗 |
| 等一个信号量 | 可能永久死锁 |

回调里只做快活（几毫秒）。慢活用 `en2m_schedule` **也不行**——
工作项跑在同一个任务上。真的慢就自己建任务，通过 `en2m_report_*` 交互。

### 8.4 `attribute_changed` 里无限递归

`attribute_changed` 跑在**触发提交的那个任务**上，在它里面再调
`en2m_attribute_set` 会**重入这个回调**。

```c
static void on_changed(const en2m_attr_path_t *path, const en2m_value_t *v, void *ctx)
{
    en2m_attribute_set(1, SOME_CLUSTER, SOME_ATTR, ...);   /* ← 小心 */
}
```

如果新值和旧值不同，就会再触发一次 `on_changed`。写成互相触发的两个属性
就是无限递归 → 栈溢出。

要么自己防重入（一个 `static bool busy`），要么用 `en2m_schedule` 把工作
挪到下一轮。见 [concurrency.md](concurrency.md#已知的注意事项)。

### 8.5 `user_ctx` 是野指针

```c
void app_main(void)
{
    my_state_t state = {0};              /* ← 栈上的局部变量 */
    cfg.user_ctx = &state;
    en2m_start(&cfg);
}                                        /* ← app_main 返回，栈被回收 */
```

`app_main` 返回后 main 任务的栈会被回收，回调里拿到的就是野指针。
`user_ctx` 必须指向 `static` 变量或者 `malloc` 出来的内存。

同样地，`cfg.mesh.name` / `.model` / `.fw` 也不能指向临时缓冲——
不过这三个会被**拷贝**进组件（`name` 最多 15 字节 + `'\0'`），
所以字符串字面量和栈上的缓冲都可以。

---

## 9. 编译和链接错误

| 错误 | 原因 | 修 |
|---|---|---|
| `fatal error: en2m.h: No such file` | `EXTRA_COMPONENT_DIRS` 没指对，或者 `main/CMakeLists.txt` 的 `REQUIRES` 里没有 `en2m` | 见 [usage.md 第 1 步](usage.md#第-1-步建工程骨架) |
| `undefined reference to en2m_start` | 同上，组件没被链接 | 同上 |
| `undefined reference to gpio_set_level` | `REQUIRES` 里缺 `driver` | 加上 |
| `undefined reference to esp_timer_*` | 缺 `esp_timer` | 加上 |
| `unknown type name 'en2m_on_off_driver_t'` | 用的是旧的 driver-ops API | 见 [migration.md](migration.md) |
| `en2m_cover_command_t` 未定义 | 同上，这个类型删了 | 换成 `en2m_command_id_t` |
| `EXTRA_COMPONENT_DIRS` 改了但没生效 | CMake 缓存 | `idf.py fullclean` |
| Kconfig 容量改了但 RAM 没变 | 容量宏是编译期的 | `idf.py fullclean && idf.py build` |
| `_Static_assert … ESP-NOW packet too large` | 改了 `EN2M_DATA_MAX` 或者帧结构 | 帧必须 ≤ 250 字节 |

`idf.py fullclean` 是改完 Kconfig / CMake 之后的标准动作，别省。

---

## 10. 组件全部日志的含义

组件很安静，只在这些地方说话。下面是**完整清单**，一条不少。

### 四个日志 tag

组件不是只用一个 tag，排错时按 tag 就能先分清是哪一层的问题：

| tag | 源文件 | 负责 |
|---|---|---|
| `en2m` | `en2m_mesh.c` | 传输层：入网、选父、转发、重传、队列 |
| `en2m_model` | `en2m_model.c` | 交互层：上报组包、命令翻译 |
| `en2m_dm` | `en2m_datamodel.c` | 数据模型：建模型、属性读写、NVS |
| `en2m_event` | `en2m_event.c` | 事件投递 |

### 正常运行会出现的（`en2m`）

| 日志 | 含义 |
|---|---|
| `mesh init` | 传输层起来了，开始找父节点 |
| `parent=<mac> cost=<n> rssi=<n>` | 挂上父节点了。**每次父节点变化都会打** |

这两条走的是内部的 `en2m_emit_log`，填了 `cfg.mesh.on_log` 之后会改走你的回调，
`ESP_LOGI` 就不打了。

### 警告（值得看，但不一定是故障）

| tag | 日志 | 含义 | 去哪 |
|---|---|---|---|
| `en2m` | `parent stale` | 到点没收到父节点的 beacon，开始重新找 | [2](#2-设备上线又掉线) |
| `en2m` | `drop uplink (not pairing / unknown)` | 协调器丢了一个陌生设备的上行 | [5.4](#54-drop-uplink-not-pairing--unknown) |
| `en2m` | `dropped N frames, the queue could not keep up` | 队列溢出 | [8.3](#83-task-watchdog-got-triggered) / 调 `QUEUE_LEN` |
| `en2m` | `command N to <mac> was never acknowledged` | 下行重传完还是没 ACK（协调器侧） | [5.3](#53-命令-ack-超时) |
| `en2m` | `no free retry slot; command N is sent unacknowledged` | 同时等 ACK 的下行太多，这条发了但不重传 | 调 `MAX_PENDING` |
| `en2m` | `en2m_mesh_loop() is obsolete: the transport runs its own task` | 旧代码在泵传输层 | [migration.md](migration.md) |
| `en2m_model` | `command payload is not valid JSON` | 下行载荷坏了 | [5.2](#52-完全没反应日志里也没动静) |
| `en2m_model` | `command payload had nothing this device understands` | JSON 合法但没有可解析的命令键 | [5.2](#52-完全没反应日志里也没动静) |
| `en2m_model` | `command <name>: no endpoint exposes cluster 0x…` | 设备上没这个 cluster | [5.2](#52-完全没反应日志里也没动静) |
| `en2m_model` | `unhandled command '<name>' on cluster 0x…` | 没人处理这条命令 | [5.2](#52-完全没反应日志里也没动静) |
| `en2m_model` | `stop needs a command handler or a window covering driver` | `STOP_MOTION` 没实现 | [5.5](#55-窗帘的停止没反应) |
| `en2m_model` | `en2m_model_loop() is obsolete: the component runs its own task` | 旧代码在泵组件。只打一次 | [migration.md](migration.md) |
| `en2m_dm` | `set N/0x…/0x…: no such attribute` | `en2m_attribute_set` 的路径不存在（拼错了，或者 cluster 没建） | 查 [data-model.md](data-model.md) |
| `en2m_dm` | `write N/0x…/0x…: no such attribute` | 同上，写路径 | 同上 |
| `en2m_dm` | `write N/0x…/0x… rejected: <err>` | 写回调返回了错误，**没有提交** | [5.1](#51-状态在-ha-里弹回去了) |
| `en2m_dm` | `nvs_open failed, attribute state will not survive a reboot` | NVS 打不开，持久化失效（其它功能正常） | [7.4](#74-nvs-坏了) |

### 错误（一定要处理）

| tag | 日志 | 含义 | 去哪 |
|---|---|---|---|
| `en2m_model` | `report exceeds 160 bytes; split the device across endpoints or trim clusters` | 三级降级都没救回来，**这次上报没发出去** | [3.1](#31-设备真的发出了上报吗) |
| `en2m_dm` | `invalid endpoint id N` | endpoint id 必须 1..254 | 代码 bug |
| `en2m_dm` | `endpoint N already exists` | 重复建同一个 endpoint | 代码 bug |
| `en2m_dm` | `no free endpoint slot (EN2M_MAX_ENDPOINTS=N)` | endpoint 用满 | [kconfig.md](kconfig.md#7-数据模型容量) |
| `en2m_dm` | `endpoint N is full, cannot add cluster 0x…` | cluster 用满 | 同上 |
| `en2m_dm` | `cluster 0x… is full, cannot add attribute 0x…` | attribute 用满 | 同上 |
| `en2m_dm` | `cluster 0x…: endpoint is NULL` | 给 `en2m_cluster_create` 传了 `NULL`——通常是上一步 `en2m_endpoint_create` 失败了没检查 | 检查返回值 |
| `en2m` | `could not create the en2m task` | 内存不够建任务 | 减小 `task_stack_size` 或者别的地方省内存 |
| `en2m` | `nvs_flash_init failed` | NVS 分区有问题，`en2m_start` 直接失败 | [7.4](#74-nvs-坏了) |

### 打开更详细的日志

```bash
idf.py menuconfig
# Component config → Log output → Default log verbosity → Debug
```

或者按 tag 单独调（注意有四个，只调 `"en2m"` 只会影响传输层）：

```c
esp_log_level_set("en2m", ESP_LOG_DEBUG);
esp_log_level_set("en2m_model", ESP_LOG_DEBUG);
esp_log_level_set("en2m_dm", ESP_LOG_DEBUG);
esp_log_level_set("en2m_event", ESP_LOG_DEBUG);
```

比调日志更有效的诊断手段通常是**接上事件**——它带结构化的 payload，
而且不需要串口。见 [events.md](events.md)。

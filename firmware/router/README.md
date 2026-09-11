# `firmware/router` — 中继节点

**没有数据模型的固件，51 行。** 只有传输层：转发上下行的帧、
重播 beacon 让更远的叶子能挂上来。它在 HA 里**不产生任何实体**。

放在 `firmware/` 而不是 `examples/` 下，因为它不是"演示怎么接硬件"的示例，
而是**要部署的东西**——信号不够的时候你会真的烧一块 C3 当中继。

| | |
|---|---|
| 角色 | `EN2M_ROLE_ROUTER` |
| 启动函数 | `en2m_mesh_init()`（**不是** `en2m_start()`） |
| 配置结构 | `en2m_config_t`（**不是** `en2m_device_config_t`） |
| endpoint / cluster | 没有 |
| 回调 | 没有，只有事件 |
| 默认名 | `router1` |
| HA 实体 | **不出实体** |

---

## 什么时候需要它

| 情况 | 要不要 |
|---|---|
| 所有设备离协调器一跳内（同层、没有厚墙） | 不需要 |
| 隔一两层楼 / 隔承重墙 | 在中间放一个 |
| 某几个设备 RSSI 长期低于 −80 dBm | 就近加一个 |
| 设备很多（> 30）挤在一个协调器上 | 分区加路由器 |

**判断依据是 RSSI，不是感觉。** 设备入网时串口上那行
`parent=aa:bb:... cost=1 rssi=-42` 里的数字：

| RSSI | 判断 |
|---|---|
| 优于 −60 dBm | 很好 |
| −60 ~ −75 dBm | 可用 |
| −75 ~ −85 dBm | 勉强，会偶发丢包和 `parent stale` |
| 差于 −85 dBm | **需要挪位置或者加一个路由器** |

RSSI 也能在 HA 里看：每个设备都有一个 `sensor.<slug>_rssi` 诊断实体。

---

## 接线

**什么都不用接，只要供电。**

但有一条硬要求：**路由器必须常电。**
它要一直收发 beacon 和转发帧，没法进深睡。
电池设备一律用 `EN2M_ROLE_LEAF`。

摆放比接线重要：放在**需要覆盖的区域和协调器之间**，
别塞进金属配电箱（基本收不到信号），天线区域不要覆铜或者压屏蔽罩。
详见 [docs/wiring.md](../../docs/wiring.md#天线与摆放)。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd firmware/router
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

**信道必须和协调器、所有设备一致**，路由器也不例外：

```bash
echo 'CONFIG_EN2M_WIFI_CHANNEL=6' >> sdkconfig.defaults
idf.py fullclean && idf.py build
```

路由器的转发量比叶子大一个数量级，所以**建议同时调大队列**：

```bash
echo 'CONFIG_EN2M_QUEUE_LEN=32' >> sdkconfig.defaults
```

另外路由器用不到数据模型，几个容量宏可以压到最小省 RAM，
见 [docs/kconfig.md](../../docs/kconfig.md#路由器节点转发量大)。

---

## 跑起来应该看到什么

```
I (598)  en2m:   mesh init
I (601)  router: router up — forwarding runs on the en2m task
I (1204) router: attached to aa:bb:cc:dd:ee:ff at cost 0 (rssi -38)
```

`attached to ... at cost 0` 说明它直接挂在协调器上（cost 0 = 协调器本身）。
`cost 1` 的话说明它挂在另一个路由器上，也能工作，但多一跳延迟。

**路由器上线之后 MQTT 上不会有任何东西。**
它没有数据模型，不上报，协调器也不会为它建设备。这是对的。

验证它真的在工作：拿一个原本 RSSI 很差的设备，
路由器上线前后对比串口里的 `parent=` 那一行——
`via` 应该从协调器的 MAC 变成路由器的 MAC，RSSI 应该明显改善。

设备侧的 `hop` 也会从 1 变成 2，在 HA 的
`sensor.<slug>_mesh_hop` 诊断实体里能直接看到。

---

## 它和设备固件的区别

```c
en2m_config_t cfg = {                    /* 不是 en2m_device_config_t */
    .role  = EN2M_ROLE_ROUTER,
    .name  = "router1",
    .model = "ex-router",
    .fw    = EN2M_FW_VERSION,
};

en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m_event, NULL);
en2m_mesh_init(&cfg);                    /* 不是 en2m_start */
```

| | 设备 | 路由器 |
|---|---|---|
| 启动函数 | `en2m_start(&device_cfg)` | `en2m_mesh_init(&mesh_cfg)` |
| 配置结构 | `en2m_device_config_t` | `en2m_config_t` |
| endpoint / cluster | 有 | 没有 |
| 回调 | 五个 | 没有（只有事件） |
| 上报 | 有 | 没有 |
| 转发 | 不转发（LEAF 只发自己的） | 上下行都转发 |
| 重播 beacon | 不 | 是，让更远的叶子能挂上来 |

`en2m_start()` 内部就是"初始化数据模型 + 调 `en2m_mesh_init()`"。
路由器不需要数据模型，所以直接调下层。
**这就是分层的实际价值：用不到的那一层可以整个不链接进来。**

S3 协调器（[espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host) 仓库的
`firmware/coordinator`）用的是同一个模式：
`en2m_mesh_init()` + 一个 `on_uplink` 回调，然后把帧转成 USB 上的 JSON。

---

## 纯事件驱动的可观测性

路由器没有属性可看，所以它的运行状况**全靠事件**：

```c
case EN2M_EVENT_PARENT_FOUND:  /* 挂上了谁，cost 多少，RSSI 多少 */
case EN2M_EVENT_PARENT_LOST:   /* 上行断了，正在找新父节点 */
case EN2M_EVENT_RX_DROPPED:    /* 队列扛不住了 */
```

**`EN2M_EVENT_RX_DROPPED` 对路由器特别重要。**
它的转发量比叶子大一个数量级，是最容易队列溢出的角色。
看到这条日志就该调大 `CONFIG_EN2M_QUEUE_LEN`。

部署的时候一定要把这个事件接上，或者至少留一根 USB 线能看串口。
十二个事件的完整清单见 [docs/events.md](../../docs/events.md)。

---

## 部署建议

1. **先测，再固定。** 拿一块 C3 插在充电宝上，放到候选位置，
   看目标设备的 RSSI 有没有改善。满意了再装到墙上。
2. **一层楼一个通常就够。** 路由器之间也能串（cost 递增），
   但每一跳都加延迟和丢包概率。**尽量不要超过两跳。**
3. **每个路由器的 `name` 要不一样**（`router1`、`router2`…）。
   虽然不产生 MQTT 设备，但日志里能分清是哪个。
4. **别把路由器和强电放一起。** 继电器吸合的干扰会明显拉低 RSSI。
5. **供电要稳。** ESP32-C3 发射瞬间峰值约 350 mA，
   劣质充电头会导致"一转发就重启"。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 路由器串口一直没有 `attached to` | 信道和协调器不一致 |
| 路由器上线了但设备还是挂在协调器上 | 正常。设备只会在协调器的信号更差时才改挂路由器 |
| MQTT 上没有路由器 | **正常**，它不产生实体 |
| 频繁 `lost the uplink` | 路由器自己离协调器太远，或者供电抖动 |
| 日志里刷 `frames dropped` | 队列太小，调大 `CONFIG_EN2M_QUEUE_LEN` |
| 加了路由器之后整网变慢 | 跳数变多了。检查是不是有设备绕了远路，必要时挪位置让它直连 |

---

## 延伸阅读

- [docs/examples.md#firmwarerouter](../../docs/examples.md#firmwarerouter) — 逐行精讲
- [docs/mesh.md](../../docs/mesh.md) — 父节点选择、cost、beacon、转发规则
- [docs/events.md](../../docs/events.md) — 全部十二个事件
- [docs/wiring.md](../../docs/wiring.md#天线与摆放) — 摆放和 RSSI 余量
- [docs/kconfig.md](../../docs/kconfig.md#路由器节点转发量大) — 路由器该怎么配

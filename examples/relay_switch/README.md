# `relay_switch` — 继电器开关

**这个仓库最应该第一个读的示例。** 112 行，把这套库的核心思想全讲完了：
远程控制和本地按键走同一条路径，应用代码里没有任务、没有轮询、没有 `while (1)`。

| | |
|---|---|
| Cluster | OnOff (`0x0006`) |
| 设备类型 | `EN2M_DEVICE_TYPE_ON_OFF_PLUG` |
| 回调 | `attribute_write` + `attribute_changed` |
| 外设组件 | [`espressif/button`](https://components.espressif.com/components/espressif/button) `^4.2.1` + 内置 `driver` 的 GPIO 输出 |
| 默认名 / slug | `relay1` |
| HA 实体 | `switch.relay1` |

---

## 接线

| GPIO | 接什么 | 说明 |
|---|---|---|
| **5** | 继电器模块 `IN` | 高电平有效（`#define RELAY_ACTIVE_HIGH true`） |
| **9** | 按键到 GND | 多数 C3 开发板上就是 **BOOT 键**，不用外接 |

引脚是 `main/main.c` 顶部的 `#define PIN_RELAY` / `#define PIN_BUTTON`，改一行就换。
换引脚前先看 [docs/wiring.md](../../docs/wiring.md#esp32-c3-引脚选择须知)——C3 上
GPIO 11–19 是碰不得的。

> **接强电前先读这一段。** `gpio_config` 跑起来之前引脚是浮空的，
> 所以**上电到固件初始化那一小段时间里继电器状态不确定**。
> 接 220 V 务必用低电平有效的继电器模块加外部下拉，或者带锁存的固态继电器。

不接任何硬件也能跑：继电器不吸合，但串口上的 `relay is now on/off` 照样打印，
HA 里的开关照样能点。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/relay_switch
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

信道要和协调器一致，改法见 [examples/README.md](../README.md#通用编译流程)。

---

## 跑起来应该看到什么

串口：

```
I (612)  en2m:      mesh init
I (615)  ex_switch: ready — the component owns the task from here
I (1843) en2m:      parent=aa:bb:cc:dd:ee:ff cost=1 rssi=-42
```

MQTT（保留消息）：

```
espnow2mqtt/relay1/availability online
espnow2mqtt/relay1/state        {"switch":"OFF","caps":["switch"],"hop":1}
```

手动控制，不经过 HA：

```bash
mosquitto_pub -t espnow2mqtt/relay1/set -m '{"switch":"ON"}'
```

串口上会出现 `relay is now on`，`state` 主题上的 `switch` 变成 `"ON"`。

按一下板载 BOOT 键，同样的两件事也会发生——**这就是这个示例的重点**。

---

## 它演示的两条控制方向

```
远程：HA → 协调器 → CMD 帧 → en2m 任务 → on_write() → gpio_set_level()
                                             └→ 返回 ESP_OK 才提交 + 上报
本地：按键回调 → en2m_schedule(toggle) → en2m 任务
                                             └→ en2m_attribute_write() → 同上
```

两条路最后都落到 `on_write()` 里的同一次 `gpio_set_level()`。
**这是故意的**：你只有一个地方碰硬件，所以不可能出现"本地按了但没上报"
或者"上报了但继电器没动"。

关键在 `en2m_attribute_write()` 和 `en2m_attribute_set()` 的区别：

- `en2m_attribute_write()` — **执行器路径**。先调 `on_write()`，
  返回 `ESP_OK` 才把新值写进数据模型并上报。硬件没动就不会有假状态发出去。
- `en2m_attribute_set()` — **传感器路径**。直接写值并上报，不过 `on_write()`。

按键走 `write` 而不是 `set`，就是为了复用那次硬件操作。
细节见 [docs/callbacks.md](../../docs/callbacks.md)。

### 按键回调里为什么要 `en2m_schedule`

```c
static void on_button(void *button_handle, void *usr_data)
{
    en2m_schedule(toggle, NULL);
}
```

`espressif/button` 不用 GPIO 中断，它**在一个 esp_timer 上轮询加消抖**
（默认 5 ms 一次，连续两次同电平才算），所以这个回调跑在任务上下文里，
不是 ISR —— 你想在这里直接调 `en2m_attribute_write()` 其实不会崩。

但还是别这么干，原因是那个 esp_timer 是**全固件所有按键共用的一个**。
在回调里取 en2m 的锁、等发包，会连带把同一块板上其它按键的消抖一起卡住。
`en2m_schedule()` 只往队列里塞一个函数指针，立刻返回，
`toggle()` 随后在 en2m 任务上运行，那里可以随便用整套 API。

**规矩很简单：按键回调里只做一件事，就是把活派出去。**

（如果你换成自己写的 GPIO 中断，那就必须用 `en2m_schedule_from_isr()`，
并且记得处理 `higher_prio_task_woken`。）

---

## 持久化是白拿的

OnOff 属性默认 `persisted = true`，所以：

1. 关灯 → 值写进 NVS
2. 断电重启
3. `en2m_start()` 从 NVS 读回来，**调一次 `on_write()`**，继电器回到关的状态
4. 第一条上报发出去的就是恢复后的真实状态

你不用写任何恢复代码。这也是为什么 `relay_init()` 必须放在
`en2m_start()` **之前**——恢复的时候引脚得已经配好了。
见 [docs/persistence.md](../../docs/persistence.md)。

---

## 换成你自己的设备

**要改的只有两处。**

1. **换执行器** — 把 `on_write()` 里的 `gpio_set_level()` 换成你的操作
   （SPI、I²C、PWM 随便）。**返回值要如实**：操作失败就返回错误码，
   组件会把这次写当作没发生，不提交也不上报。
2. **换名字** — `cfg.mesh.name` 决定 MQTT slug 和 HA 里的实体 id，
   `cfg.mesh.model` 会显示在 HA 设备页上。

想控制多路继电器就加 endpoint：

```c
en2m_endpoint_create_device(2, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
```

`on_write()` 里用 `path->endpoint_id` 分路。默认最多 4 个 endpoint，
上限见 [docs/kconfig.md](../../docs/kconfig.md)。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 串口没有 `parent=` | 信道和协调器不一致（99% 是这个） |
| 有 `parent=` 但 MQTT 上没东西 | 没开 `permit_join`，Bridge 日志里有 `drop uplink` |
| HA 里点了没反应，串口也没日志 | `base_topic` 两边不一致 |
| 一按键就重启 | 供电不够，继电器线圈拉垮了电源。见 [docs/wiring.md](../../docs/wiring.md#供电) |
| 按一次跳两下 | 消抖不够。`idf.py menuconfig` → `Component config` → `Button` 里把 `BUTTON_PERIOD_TIME_MS` 调大 |
| 按键完全没反应 | 按键默认低电平有效（`BUTTON_ACTIVE_LEVEL 0`）。上拉到 VCC 的按键要改成 `1` |

---

## 延伸阅读

- [docs/examples.md#relay_switch](../../docs/examples.md#relay_switch) — 逐行精讲
- [docs/state-flow.md](../../docs/state-flow.md) — 一次点击从 HA 到继电器的全链路
- [docs/callbacks.md](../../docs/callbacks.md) — `write` 和 `changed` 的确切语义

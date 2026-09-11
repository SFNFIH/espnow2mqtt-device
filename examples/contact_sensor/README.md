# `contact_sensor` — 门磁 / 干簧管

**事件驱动 + 周期性自愈的示例。** 状态变化走边沿回调（立刻上报，没有周期延迟），
同时留一个读回调当兜底（每次周期上报时重新对一次硬件）。
**任何"状态是事件来的"传感器都该抄这一份**：门磁、水浸、按键式限位、烟感。

| | |
|---|---|
| Cluster | Boolean State (`0x0045`) |
| 设备类型 | `EN2M_DEVICE_TYPE_CONTACT_SENSOR` |
| 回调 | `attribute_read`（兜底）+ 边沿推送 |
| 外设组件 | [`espressif/button`](https://components.espressif.com/components/espressif/button) `^4.2.1` |
| 默认名 / slug | `door1` |
| HA 实体 | `binary_sensor.door1_contact`（设备类别 `door`） |

---

## 接线

| GPIO | 接什么 | 说明 |
|---|---|---|
| **9** | 干簧管 / 门磁的一端到 GND | **低电平有效**，用芯片内部上拉 |

```
   GPIO9 ──┬──── 干簧管 ────  GND
            │
        （内部上拉到 3V3，不用外接电阻）
```

内部上拉由 `espressif/button` 根据 `active_level = 0` 自动打开，
你不用自己 `gpio_config`。

不接任何东西的话 GPIO9 就是板载 BOOT 键，**按一下等于"门开了"**，
拿它先验证链路最方便。

### 极性

```c
#define CONTACT_ACTIVE_LEVEL 0        /* 吸合时引脚是低电平 */
#define CONTACT_OPEN_WHEN_ACTIVE true /* 吸合算"门开" */
```

上面那句 `CONTACT_OPEN_WHEN_ACTIVE true` 的意思是
**接通（拉到 GND）算"门开"**。绝大多数门磁是
"磁铁靠近时触点闭合"，也就是关门闭合、开门断开——
那种要把它改成 `false`，不然 HA 里会全部反过来。

**判断方法**：烧好之后拿磁铁靠近再拿开，看 MQTT 上的 `contact` 是不是
"开门 → ON"。反了就翻转这个宏，不要去改 HA 的模板——
那样一个设备一个补丁，很快就乱。

常开（NO）和常闭（NC）两种门磁都能用，区别只在这个宏。
两个宏为什么要分开，见下面[极性两个宏都可配](#极性两个宏都可配)。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/contact_sensor
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

第一次 `build` 会联网把 `espressif/button` 下载到本工程的
`managed_components/`。

---

## 跑起来应该看到什么

```
espnow2mqtt/door1/availability online
espnow2mqtt/door1/state         {"contact":"ON","caps":["contact"],"hop":1}
```

`"ON"` = 断开 = 门开。`"OFF"` = 闭合 = 门关。

按一下 BOOT 键（或者动一下磁铁），**几十毫秒内**就有新的 `state` 出来。
不是等下一个周期上报——这就是这个示例的重点。

---

## 为什么要两条路

```
路径 A（快）：边沿回调 → en2m_schedule(publish_contact)
                        → contact_is_open() → en2m_report_boolean_state()
                        → 立刻上报

路径 B（慢，兜底）：上报定时器到 → on_read() → contact_is_open()
                        → 值进这一轮上报
```

看起来重复，其实两条路解决的是两个不同的问题：

| 路径 | 解决什么 | 不能解决什么 |
|---|---|---|
| A 边沿 | **延迟**。开门到 HA 里变色 < 100 ms | 帧丢了就丢了 |
| B 读回调 | **一致性**。丢帧、设备重启、协调器重启之后自动对齐 | 延迟（要等下一个周期） |

只有 A 的话：一次射频丢包就会让 HA 里的状态和现实**永久**不一致，
直到下一次开关门。ESP-NOW 是无确认的广播式传输，丢包不是假设，是必然。

只有 B 的话：延迟等于上报周期，门磁基本没用了。

**两条一起才是对的。** 这个"事件推 + 周期自愈"的组合是做无线传感器的标准做法，
Zigbee 的设备也是这么干的。

---

## 干簧管为什么用按键组件

一个干簧管在电路上就是一个按键：一根线，两个电平，会抖。
所以这里直接用 [`espressif/button`](https://components.espressif.com/components/espressif/button)，
把 `BUTTON_PRESS_DOWN` 和 `BUTTON_PRESS_UP` 两个事件都注册上，
每个边沿都能拿到一次**已经消抖过的**回调：

```c
iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_contact);
iot_button_register_cb(s_contact, BUTTON_PRESS_DOWN, NULL, on_contact_edge, NULL);
iot_button_register_cb(s_contact, BUTTON_PRESS_UP,   NULL, on_contact_edge, NULL);
```

"按下"和"松开"在门磁的语境里就是"吸合"和"断开"，
两个事件挂**同一个**回调，因为回调根本不看是哪个边沿——见下一节。

不用自己写 GPIO 中断的好处除了省代码，还有消抖是现成的：
组件在一个 esp_timer 上 5 ms 轮询一次，连续两次同电平才认。
机械干簧管抖 1–5 ms，默认参数刚好够；
SW-520D 这类振动开关能抖到 50 ms，就要去
`menuconfig` → `Component config` → `Button` 里把周期调大。

## 回调里为什么要回读硬件

```c
static bool contact_is_open(void)
{
    return (iot_button_get_key_level(s_contact) == BUTTON_ACTIVE) == CONTACT_OPEN_WHEN_ACTIVE;
}

static void publish_contact(void *arg)
{
    en2m_report_boolean_state(ENDPOINT, contact_is_open());
}
```

注意它**没有**用"上次是 ON 那这次就是 OFF"这种取反逻辑，
也没有去分辨触发它的是 `PRESS_DOWN` 还是 `PRESS_UP`，
而是老老实实回读一次电平。

理由：边沿可能因为抖动**连着来两次**，也可能在处理的时候错过一次。
取反逻辑一旦错过一次就永久反相，回读则自动纠正。
这几微秒的 GPIO 读取比可能的永久错误便宜太多。

`iot_button_get_key_level()` 就是一次 `gpio_get_level()`，
不经过消抖状态机，所以任何任务都能调，也没有延迟。

### 极性两个宏都可配

```c
#define CONTACT_ACTIVE_LEVEL 0        /* 干簧管吸合时引脚是低还是高 */
#define CONTACT_OPEN_WHEN_ACTIVE true /* 吸合算"门开"还是"门关" */
```

第一个是**电气极性**（上拉到 VCC 还是下拉到 GND），
第二个是**语义极性**（磁铁靠近是开门还是关门，取决于你把磁铁装在哪）。
这两件事互相独立，所以分成两个宏。接反了改一个 `true`/`false` 就行。

### 注册顺序

```c
contact_init();                      // 1. 先把按键配好
en2m_endpoint_create_device(...);    // 2. 再建数据模型
en2m_start(&cfg);                    // 3. 最后启动
```

`contact_init()` 在 `en2m_start()` 之前，所以理论上存在一个极短的窗口：
按键回调已经能跑，但 en2m 的队列还没建。
这不会崩——`en2m_schedule()` 发现队列是 `NULL` 会直接返回
`ESP_ERR_INVALID_STATE`，那一次事件被丢掉而已，
下一轮周期上报的路径 B 会把状态补齐。

## 换成你自己的传感器

这份代码改一个宏就能变成好几种设备：

| 做什么 | 改什么 |
|---|---|
| 水浸传感器 | `EN2M_DEVICE_TYPE_CONTACT_SENSOR` 不变，`cfg.mesh.name = "leak1"`。HA 里改 device class 就是水浸图标 |
| 窗磁 | 什么都不用改，改个名字 |
| 邮箱 / 抽屉 | 同上 |
| 振动 / 倾倒（SW-520D） | 加长消抖：`menuconfig` 里把 `BUTTON_PERIOD_TIME_MS` 调大 |
| 烟感（有干接点输出的） | 换成 `EN2M_CLUSTER_SMOKE_CO`，用 `en2m_report_smoke()` |

**消抖**：`espressif/button` 默认 5 ms 轮询、连续两次同电平才认，
也就是大约 10 ms 的消抖窗口。
机械干簧管抖动 1–5 ms，够用；SW-520D 这类振动开关能抖到 50 ms，要调。
抖动太大的表现是一次动作在 MQTT 上出现好几条上报。

---

## 电池供电

门磁是最典型的电池设备。这个示例还是常电写法，
要做电池版改两处：

```c
.report_mode = EN2M_REPORT_ON_CHANGE_ONLY,   // 不要周期上报
```

再用 **GPIO 唤醒的深睡**（`esp_deep_sleep_enable_gpio_wakeup`）代替常驻轮询。
`espressif/button` 自己也有省电模式（`button_gpio_config_t.enable_power_save`），
让它在没有按键活动的时候停掉那个轮询定时器，改用电平中断唤醒。
代价是丢掉了路径 B 的自愈能力，所以通常折中成
"深睡 + 每小时醒一次发个心跳"。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| `contact` 和现实完全反的 | `CONTACT_ACTIVE_LOW` 设反了，见上面"极性" |
| 一次开门出来三四条上报 | 消抖窗口太短 |
| 上报正常但 HA 里图标是通用的开关 | HA 那边的 device class 靠 `caps` 里的 `contact` 推断，检查 `caps` 有没有发出去 |
| 门磁远离协调器就不灵 | ESP-NOW 丢包。加一个 [`firmware/router`](../../firmware/router) 中继，或者看 RSSI 挪位置 |
| 不接门磁时一直是 ON | 正常：GPIO9 悬空被内部上拉到高，就是"断开" |

---

## 延伸阅读

- [docs/examples.md#contact_sensor](../../docs/examples.md#contact_sensor) — 逐行精讲
- [docs/concurrency.md](../../docs/concurrency.md) — 哪个回调跑在哪个任务上
- [docs/reporting.md](../../docs/reporting.md) — 事件上报和周期上报怎么共存

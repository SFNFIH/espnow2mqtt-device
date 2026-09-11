# `scene_switch` — 无线按键 / 场景开关

**一个回调都没有的示例。** 纯上行、无状态，只有按键往外发。
也是**电池设备的模板**：射频只在真有人按的时候醒。

按键在这套系统里有一个特殊问题，值得先读懂再抄：
**按键没有状态**。上报送到的时候，"按"这个动作已经结束了。
这个示例展示的就是怎么用一个**状态协议**表达一个**事件**。

| | |
|---|---|
| Cluster | Switch (`0x003B`) |
| 设备类型 | `EN2M_DEVICE_TYPE_GENERIC_SWITCH` |
| 回调 | **没有**（只有驱动的手势回调） |
| 驱动 | `drv_gpio_button_gesture` |
| 上报模式 | `EN2M_REPORT_ON_CHANGE_ONLY` |
| 默认名 / slug | `switch1` |
| HA 实体 | `event.switch1_button` |

---

## 接线

| GPIO | 接什么 | 说明 |
|---|---|---|
| **9** | 按键到 GND | 多数 C3 开发板上就是 **BOOT 键**，不用外接 |

```c
#define BUTTON_ACTIVE_LOW true      // 按下拉到 GND
#define DEBOUNCE_MS 30              // 消抖窗口
#define LONG_PRESS_MS 800           // 按住多久算长按
#define DOUBLE_GAP_MS 300           // 两次按间隔小于这个算双击
```

不接任何东西就能测：按板载 BOOT 键。

要接多个按键（四键场景开关）见下文"做成多按键"。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/scene_switch
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

串口：

```
I (615)  ex_scene: ready — press, double press, or hold GPIO9
I (8342) ex_scene: gesture 0
```

MQTT——**按下第一个键之前，`state` 里什么都没有**：

```
espnow2mqtt/switch1/state  {"hop":1,"via":"..."}
```

这是故意的。序列化时只有 `button > 0` 才会报 `button` 和 `button_action`，
所以计数还是 0 的新设备不会声明 `button` 这个 cap，
**HA 里也就还没有实体**。按一下就出来了。

MQTT——**按一下**：

```
espnow2mqtt/switch1/state  {"button":1,"button_action":"press","caps":["button"],"hop":1}
```

**再按一下**：

```
espnow2mqtt/switch1/state  {"button":2,"button_action":"press","caps":["button"],"hop":1}
```

注意 `button` 从 1 变成了 2。**这个计数器就是整个设计的关键。**

四种手势：

| 操作 | `button_action` |
|---|---|
| 短按 | `press` |
| 双击 | `double_press` |
| 长按（到 800 ms 时立刻发） | `long_press` |
| 长按之后松手 | `release` |

---

## 为什么需要一个计数器

这套协议的上行有两个特点：

1. **每条上行是一份完整状态快照**，不是增量
2. **`state` 主题是 retained 的**，订阅者一连上就能拿到最后一条

对温度、开关这类**有状态**的东西来说这两点都是优点。
但按键没有状态，如果只发 `{"button_action":"press"}`：

- 你按两次，两条一模一样的消息
- 接收方**无法区分"按了两次"和"一条消息重复投递了"**
- 更糟：HA 重启后订阅 retained 主题，会收到那条旧消息，**凭空触发一次事件**

所以每次按键都要**带一个单调递增的计数**。HA 集成的逻辑是
"`button` 的值变了 → 触发一次 `event` 实体"，
值没变就什么都不做——重复投递和 retained 重放都被自动吃掉了。

Matter 在这里用的是**事件**（Switch cluster 的 `InitialPress` 等等），
而这套协议里没有事件的概念，所以只能用状态表达。
两个非 Matter 属性就是为这个加的：

| 属性 | ID | 说明 |
|---|---|---|
| `EN2M_ATTR_PRESS_COUNT` | `0xFF00` | 单调递增的按键计数，**持久化** |
| `EN2M_ATTR_PRESS_ACTION` | `0xFF01` | 最后一次按键的种类 |

`0xFFxx` 这段是厂商自定义区间，不会和 Matter 的标准属性撞号。
完整讨论见 [docs/data-model.md](../../docs/data-model.md#switch-cluster-为什么有两个非-matter-属性)。

### 计数器为什么要持久化

**因为重启归零会被读成"又按了一次"。**

计数器存在 NVS 里，每次重启从上次的值接着数。
不持久化的话每次断电重启都会从 0 跳到 1，HA 那边看到值变了，
就凭空触发一次场景——半夜一次掉电能让全屋的灯亮起来。

### 计数器不用你自己管

```c
en2m_report_button(ENDPOINT, action);
```

这一个调用做了三件事：读出当前计数、**先写 action、后写计数 +1**。

顺序是故意的：计数才是触发上报的那个值，所以它必须最后变，
否则接收方可能看到"新计数 + 上一次的 action"。

---

## 手势识别在驱动里，不在应用里

`main.c` 里的 `on_gesture()` 只做一件事：把驱动的手势枚举翻译成
`en2m_press_action_t`，然后调 `en2m_report_button()`。
短按 / 双击 / 长按的**状态机在 `drivers/drv_gpio_button_gesture.c` 里**。

那个驱动的分工是：

- **ISR** 只消抖 + 把边沿塞进队列（而且是**回读电平**，不靠记极性——
  漏一个边沿也能自动纠正）
- **一个任务**（`btn_gesture`，2560 字节栈）跑状态机，
  用队列接收的**超时当作时钟**，所以不需要额外的定时器

状态机：

```
IDLE        --按下-->   PRESSED      (等 long_ms)
PRESSED     --松开-->   WAIT_DOUBLE  (等 double_gap_ms)
PRESSED     --超时-->   发 LONG,  LONG_HELD
WAIT_DOUBLE --按下-->   发 DOUBLE, CONSUMED
WAIT_DOUBLE --超时-->   发 SHORT,  IDLE
LONG_HELD   --松开-->   发 RELEASE, IDLE
CONSUMED    --松开-->   IDLE
```

**短按有 `DOUBLE_GAP_MS`（300 ms）的固有延迟**，这是双击检测的代价：
不等 300 ms 就没法知道后面还有没有第二下。不需要双击的话把
`double_gap_ms` 传 0，短按就变成即时的。

**这是整个仓库里唯一一个用到任务的示例，而那个任务在驱动里，
应用代码里还是一行任务都没有。**

---

## 为什么 `report_mode` 是 `ON_CHANGE_ONLY`

```c
.report_mode = EN2M_REPORT_ON_CHANGE_ONLY,
```

默认模式会周期性地重发当前状态（给丢包兜底）。对按键来说这没有意义：

- 按键计数不会自己变，周期上报只是在**重发同一个数**
- HA 那边看到值没变，什么都不做
- 但射频每次都得醒一下

场景开关基本都是电池供电的，`ON_CHANGE_ONLY` 让它**只在真有人按的时候发射**。
一颗 CR2032 能撑很久。

各上报模式的语义见 [docs/reporting.md](../../docs/reporting.md)。

---

## 在 HA 里怎么用

出来的是一个 **`event` 实体**（`event.switch1_button`），不是 `sensor`。
`event` 实体的好处是它天生就是"一次性事件"语义——
不会像 `sensor` 那样在重启后保留一个旧值然后被自动化误触发。

自动化里这么写：

```yaml
automation:
  - alias: 场景开关双击关全屋
    triggers:
      - trigger: state
        entity_id: event.switch1_button
        attribute: event_type
        to: double_press
    actions:
      - action: light.turn_off
        target:
          entity_id: all
```

或者用 UI 建自动化，触发器选"事件实体"，事件类型下拉里有那四种。

四种 `event_type` 就是上面那张表里的 `press` / `double_press` /
`long_press` / `release`。

---

## 做成多按键

四键场景开关的正确做法是**一个按键一个 endpoint**：

```c
#define EP_BTN1 1
#define EP_BTN2 2
#define EP_BTN3 3
#define EP_BTN4 4

static void on_gesture_ep(drv_gpio_button_gesture_t gesture, void *ctx)
{
    uint8_t ep = (uint8_t)(uintptr_t)ctx;      // ← ctx 带着 endpoint 号
    en2m_report_button(ep, translate(gesture));
}

void app_main(void)
{
    for (int i = 0; i < 4; i++) {
        en2m_endpoint_create_device(EP_BTN1 + i, EN2M_DEVICE_TYPE_GENERIC_SWITCH);
        drv_gpio_button_gesture_init(pins[i], true, DEBOUNCE_MS, LONG_PRESS_MS,
                                     DOUBLE_GAP_MS, on_gesture_ep,
                                     (void *)(uintptr_t)(EP_BTN1 + i));
    }
    ...
}
```

**默认最多 4 个 endpoint**（`CONFIG_EN2M_MAX_ENDPOINTS`），
六键的话要调这个配置，见 [docs/kconfig.md](../../docs/kconfig.md)。

注意每个 `drv_gpio_button_gesture_init()` 会起**一个自己的任务**，
四个按键就是四个 2560 字节的栈（约 10 KB）。
按键多了应该改成一个任务轮询所有按键——这时候直接改驱动更划算。

---

## 换成真硬件

| 用什么 | 要改什么 |
|---|---|
| 普通轻触开关 | 什么都不用改 |
| 自锁 / 船型开关 | 手势识别没意义了，改成 [`contact_sensor`](../contact_sensor) 那种电平上报 |
| 触摸按键（TTP223） | 一样接 GPIO，但消抖可以调小（芯片已经处理过） |
| 电容触摸（C3 无触摸外设） | C3 没有触摸控制器，要用外部芯片 |

**电池版还要加深睡**：`esp_deep_sleep_enable_gpio_wakeup()` 让按键把芯片唤醒，
然后 `en2m_start()` → 上报 → 回去睡。
注意入网要几十到几百毫秒，所以按下到 HA 里响应会有这个延迟。
追求手感就别深睡，用 light sleep。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 一按出来好几条上报 | 按键抖动，加大 `DEBOUNCE_MS` |
| 短按总是被识别成双击 | `DOUBLE_GAP_MS` 太大，或者按键在松手时抖动 |
| 长按识别不出来 | `LONG_PRESS_MS` 比你实际按的时间长 |
| 短按感觉慢半拍 | 正常，那是 `DOUBLE_GAP_MS`。不要双击就设 0 |
| HA 重启后自动化被触发一次 | **不该发生**。如果发生了，说明计数器没持久化，检查 NVS 分区 |
| HA 里没有 `event` 实体 | 新设备在第一次按键之前不声明 `button` cap，按一下就有了；还没有的话是集成版本太老，`event` 平台需要 0.4.0 及以上 |

---

## 延伸阅读

- [docs/examples.md#scene_switch](../../docs/examples.md#scene_switch) — 逐行精讲
- [docs/data-model.md](../../docs/data-model.md#switch-cluster-为什么有两个非-matter-属性) — 两个非 Matter 属性的来由
- [docs/reporting.md](../../docs/reporting.md) — `EN2M_REPORT_ON_CHANGE_ONLY`

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
| 回调 | **没有**（只有按键组件的手势回调） |
| 外设组件 | [`espressif/button`](https://components.espressif.com/components/espressif/button) `^4.2.1` |
| 上报模式 | `EN2M_REPORT_ON_CHANGE_ONLY` |
| 默认名 / slug | `switch1` |
| HA 实体 | `event.switch1_button` |

---

## 接线

| GPIO | 接什么 | 说明 |
|---|---|---|
| **9** | 按键到 GND | 多数 C3 开发板上就是 **BOOT 键**，不用外接 |

```c
#define BUTTON_ACTIVE_LEVEL 0       // 按下拉到 GND
#define CLICK_GAP_MS 300            // 松手后等多久才确定"没有下一击"
#define LONG_PRESS_MS 800           // 按住多久算长按
```

消抖不在这里配，它是 `espressif/button` 的全局配置项
（`menuconfig` → `Component config` → `Button` → `BUTTON_PERIOD_TIME_MS`，
默认 5 ms 轮询、连续两次同电平才认，约 10 ms 窗口）。

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

第一次 `build` 会联网把 `espressif/button` 下载到本工程的
`managed_components/`。

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

## 手势识别在组件里，不在应用里

四种手势——单击、双击、长按、释放——**全部由 `espressif/button` 原生提供**，
应用侧只是把它的事件映射到 `en2m_press_action_t`：

```c
static const struct {
    button_event_t event;
    en2m_press_action_t action;
} map[] = {
    {BUTTON_SINGLE_CLICK,     EN2M_PRESS_SHORT},
    {BUTTON_DOUBLE_CLICK,     EN2M_PRESS_DOUBLE},
    {BUTTON_LONG_PRESS_START, EN2M_PRESS_LONG},
    {BUTTON_LONG_PRESS_UP,    EN2M_PRESS_RELEASE},
};

for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    iot_button_register_cb(btn, map[i].event, NULL, on_press,
                           (void *)(uintptr_t)map[i].action);
}
```

**一个回调服务四个事件**，因为要发什么 action 是通过 `usr_data`
传进来的，回调里不用 `switch`：

```c
static void on_press(void *button_handle, void *usr_data)
{
    en2m_schedule(report_press, usr_data);
}
```

`usr_data` 原封不动地传给 `en2m_schedule()` 的 `arg`，
所以整条链路上一个分支都没有。

### 为什么要经过 `en2m_schedule`

`en2m_report_button()` 内部是"读计数 → 写 action → 写计数 +1"，
**三步不是原子的**。全部收拢到 en2m 任务上执行，计数就不可能被撕裂。

而且 `espressif/button` 的回调跑在一个**全固件共用的 esp_timer** 上，
在那里面等锁、等发包会连带卡住同一块板上其它按键的消抖。
回调里只派活、不干活。

### 手势时间参数

```c
const button_config_t btn_cfg = {
    .long_press_time = LONG_PRESS_MS,  // 800 ms
    .short_press_time = CLICK_GAP_MS,  // 300 ms
};
```

留空（`{0}`）的话组件用自己的默认值（长按 1500 ms、`short_press_time` 180 ms）。
这个示例把长按调到 800 ms，因为 1.5 秒对场景开关来说手感偏迟钝。

**`short_press_time` 这个名字有点误导**：它不是"按多短算短按"，
而是**松手之后等待下一击的窗口**。看状态机就清楚了
（`iot_button.c` 的 `PRESS_REPEAT_DOWN_CHECK`）：

```
按下 ──> PRESS_DOWN
松开 ──> PRESS_UP，开始计时
          ├─ 在 short_press_time 内又按下  ──> repeat++，继续等
          └─ 超过 short_press_time 没动作  ──> 按 repeat 发
                                               1 次 = SINGLE_CLICK
                                               2 次 = DOUBLE_CLICK
```

所以这个值同时决定两件事：

1. **双击能有多慢**。两击之间超过 300 ms 就变成两次单击。
   默认的 180 ms 对不少人来说偏紧，所以这里放宽到 300 ms。
2. **单击有多慢**。组件必须等满这个窗口才能确定"后面没有第二下了"，
   所以**单击必然延迟 300 ms**。这是双击检测的固有代价，不是 bug。

不需要双击的话就别注册 `BUTTON_DOUBLE_CLICK` ——
不过要注意组件**照样会等**那个窗口（状态机是一样的），
真要即时响应得改用 `BUTTON_PRESS_DOWN`，在按下的瞬间就发。
`relay_switch` 里的本地按键如果嫌慢，就是这么改。

**`LONG_PRESS_MS` 必须大于 `CLICK_GAP_MS`。** 组件在
`iot_button_register_cb()` 里会检查这一条，不满足的话注册
`BUTTON_LONG_PRESS_START` 会返回 `ESP_ERR_INVALID_ARG`——
在这个示例里就是 `ESP_ERROR_CHECK` 直接挂掉。
道理也说得通：连击窗口还没过，"长按"就无从谈起。

**还有一个容易踩的**：双击的**第二下**如果按住超过 `short_press_time`，
状态机会走到 `PRESS_END` 而不发 `DOUBLE_CLICK`。
也就是说"快按一下、再按住"这个动作什么都不会发。
长按要单独用 `BUTTON_LONG_PRESS_START`，别指望它和连击混着用。

### 还有别的事件可用

`iot_button.h` 里一共 11 种事件，这个示例只用了 4 种。另外几种里有用的：

| 事件 | 什么时候来 | 能做什么 |
|---|---|---|
| `BUTTON_LONG_PRESS_HOLD` | 长按期间**反复**触发 | 长按调亮度（一直加） |
| `BUTTON_MULTIPLE_CLICK` | 指定次数的连击 | 五连击进配网模式 |
| `BUTTON_PRESS_REPEAT` | 每次连击都来，带次数 | 自己数几连击 |

**这个仓库里唯一一个用到任务的示例，现在连那个任务也没有了**——
手势状态机跑在 `espressif/button` 和全固件共用的那个 esp_timer 上，
不再是每个按键一个 2560 字节栈的任务。

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
static const int pins[4] = {2, 3, 9, 10};

/* usr_data 里同时塞 endpoint 和 action */
#define PACK(ep, action) ((void *)(uintptr_t)(((ep) << 8) | (action)))

static void report_press(void *arg)
{
    uintptr_t packed = (uintptr_t)arg;
    en2m_report_button((uint8_t)(packed >> 8), (en2m_press_action_t)(packed & 0xFF));
}

void app_main(void)
{
    for (int i = 0; i < 4; i++) {
        uint8_t ep = EP_BTN1 + i;
        button_handle_t btn = NULL;
        const button_gpio_config_t gpio_cfg = {.gpio_num = pins[i], .active_level = 0};

        en2m_endpoint_create_device(ep, EN2M_DEVICE_TYPE_GENERIC_SWITCH);
        iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
        iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_press,
                               PACK(ep, EN2M_PRESS_SHORT));
        /* ……其余三个事件同理 */
    }
    ...
}
```

**默认最多 4 个 endpoint**（`CONFIG_EN2M_MAX_ENDPOINTS`），
六键的话要调这个配置，见 [docs/kconfig.md](../../docs/kconfig.md)。

**多按键的开销几乎是零。** `espressif/button` 的所有实例
**共用同一个 esp_timer**，四个按键不是四个任务、四个定时器，
就是同一个 5 ms 回调里多扫三个引脚。
这也是为什么按键回调里绝对不能阻塞：卡住一个就卡住全部。

（按键实例挂在一个链表上，数量没有上限，只受内存限制。）

---

## 换成真硬件

| 用什么 | 要改什么 |
|---|---|
| 普通轻触开关 | 什么都不用改 |
| 自锁 / 船型开关 | 手势识别没意义了，改成 [`contact_sensor`](../contact_sensor) 那种电平上报 |
| 触摸按键（TTP223） | 一样接 GPIO，输出是高有效，把 `BUTTON_ACTIVE_LEVEL` 改成 `1` |
| 电容触摸（C3 无触摸外设） | C3 没有触摸控制器，要用外部芯片 |

**电池版还要加深睡**：`espressif/button` 自己有省电模式
（`button_gpio_config_t.enable_power_save`），空闲时停掉轮询定时器、
改用电平中断唤醒，配合 `esp_deep_sleep_enable_gpio_wakeup()` 让按键把芯片唤醒，
然后 `en2m_start()` → 上报 → 回去睡。
注意入网要几十到几百毫秒，所以按下到 HA 里响应会有这个延迟。
追求手感就别深睡，用 light sleep。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 一按出来好几条上报 | 按键抖动，`menuconfig` 里加大 `BUTTON_PERIOD_TIME_MS` |
| 短按总是被识别成双击 | 按键在松手时抖动，同上 |
| 长按识别不出来 | `LONG_PRESS_MS` 比你实际按的时间长 |
| 单击慢半拍 | 正常，组件在等 `CLICK_GAP_MS`（300 ms）。调小或者改用 `BUTTON_PRESS_DOWN` |
| 双击老是被当成两次单击 | 两击间隔超了 `CLICK_GAP_MS`，往上调 |
| "点一下再按住"什么都不发 | 状态机的已知行为，见上面手势时间参数那一节 |
| 启动就挂在 `button_init()` | `LONG_PRESS_MS` 设得比 `CLICK_GAP_MS` 还小 |
| HA 重启后自动化被触发一次 | **不该发生**。如果发生了，说明计数器没持久化，检查 NVS 分区 |
| HA 里没有 `event` 实体 | 新设备在第一次按键之前不声明 `button` cap，按一下就有了；还没有的话是集成版本太老，`event` 平台需要 0.4.0 及以上 |

---

## 延伸阅读

- [docs/examples.md#scene_switch](../../docs/examples.md#scene_switch) — 逐行精讲
- [docs/data-model.md](../../docs/data-model.md#switch-cluster-为什么有两个非-matter-属性) — 两个非 Matter 属性的来由
- [docs/reporting.md](../../docs/reporting.md) — `EN2M_REPORT_ON_CHANGE_ONLY`

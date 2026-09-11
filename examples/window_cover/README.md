# `window_cover` — 窗帘 / 卷帘

**慢执行器的示例，也是唯一一个用命令回调而不是写回调的示例。**

电机不会瞬间到位。窗帘从全开走到全关要十几秒，
这十几秒里 HA 应该看到位置在**连续变化**，而不是"发出命令 → 立刻显示 100%"。
写回调做不到这件事，命令回调可以。

| | |
|---|---|
| Cluster | Window Covering (`0x0102`) |
| 设备类型 | `EN2M_DEVICE_TYPE_WINDOW_COVERING` |
| 回调 | 只有 `command` |
| 驱动 | 无（`esp_timer` 模拟行程） |
| 默认名 / slug | `cover1` |
| HA 实体 | `cover.cover1` |

---

## 接线

**这个示例不接任何硬件。** 行程是一个 200 ms 走 5% 的 `esp_timer` 模拟的，
所以全程 4 秒。真电机的接法见"换成真硬件"。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/window_cover
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

```bash
mosquitto_pub -t espnow2mqtt/cover1/set -m '{"cover":"CLOSE"}'
```

串口：

```
I (5102) ex_cover: moving 0% -> 100%
I (9310) ex_cover: stopped at 100%
```

MQTT——**中间的每一步都会发出来**：

```
espnow2mqtt/cover1/state  {"position":5,"cover":"OPEN","caps":["cover"],"hop":1}
espnow2mqtt/cover1/state  {"position":10,"cover":"OPEN",...}
...
espnow2mqtt/cover1/state  {"position":95,"cover":"CLOSED",...}
espnow2mqtt/cover1/state  {"position":100,"cover":"CLOSED",...}
```

HA 里的窗帘控件会跟着动。四条命令：

```bash
mosquitto_pub -t espnow2mqtt/cover1/set -m '{"cover":"OPEN"}'
mosquitto_pub -t espnow2mqtt/cover1/set -m '{"cover":"CLOSE"}'
mosquitto_pub -t espnow2mqtt/cover1/set -m '{"cover":"STOP"}'
mosquitto_pub -t espnow2mqtt/cover1/set -m '{"position":40}'
```

---

## 位置约定

**`position` 是"关了多少百分比"，不是"开了多少"。**

| `position` | 含义 | `cover` 字段 |
|---|---|---|
| 0 | 全开 | `OPEN` |
| 40 | 关了 40% | `OPEN` |
| 95–100 | 基本关上 | `CLOSED` |

这是 Matter 的约定（`CurrentPositionLiftPercentage`）。
**HA 的约定正好相反**：HA 里 100 是全开。

这个反转**在 HA 集成里做了**，固件只管 Matter 的那一套，
不要在固件里翻转，那样两边都会翻一次等于没翻。

`cover` 字段是序列化时顺手加的一个**布尔版位置**（`>= 95` 算关），
给不关心百分比的场合用。

---

## 为什么不用写回调

对比一下两种回调在慢执行器上的表现：

```
写回调：HA 发 {"position":100} → on_write() 被调
                              → 你要在这里面等十几秒？（不行，会堵住 en2m 任务）
                              → 或者立刻返回 ESP_OK？（那 HA 立刻显示 100%，是假的）

命令回调：HA 发 {"cover":"CLOSE"} → on_command() 被调 → 启动电机 → 立刻返回
                                 → 行程定时器每 200 ms 调一次
                                   en2m_report_cover_position(实际位置)
                                 → HA 看到位置一格一格在动
```

关键区别：

| | 写回调 | 命令回调 |
|---|---|---|
| 语义 | "把这个属性设成这个值" | "做这件事" |
| 属性什么时候变 | 回调返回 `ESP_OK` 时**自动**提交 | **不自动**，要你自己 `en2m_report_*` |
| 适合 | 瞬间到位的执行器 | 需要时间的动作、没有对应属性的动作 |

命令回调**不会自动改数据模型**，这正是我们要的：
位置只在电机真的走过去之后才更新。

这个模式适用于任何"慢动作"：窗帘、电动阀门、车库门、投影幕、
以及任何"启动一个过程"的操作（自检、标定、清洗）。

### 四条命令都要实现

```c
case EN2M_CMD_UP_OR_OPEN:            motor_go(0);   return ESP_OK;
case EN2M_CMD_DOWN_OR_CLOSE:         motor_go(100); return ESP_OK;
case EN2M_CMD_GO_TO_LIFT_PERCENTAGE: motor_go((uint8_t)en2m_value_as_int(&command->arg));
                                                    return ESP_OK;
case EN2M_CMD_STOP_MOTION:           motor_stop();  return ESP_OK;
```

**`STOP_MOTION` 是最重要的那一条**，别偷懒。窗帘卡住、
撞到花盆、或者用户就是想停在一半——没有 STOP 就只能等它走完或者拉闸。

`motor_stop()` 里那句 `s_cover.target = s_cover.position` 是必须的：
不把目标改成当前位置，下一个定时器 tick 又会接着走。

### 行程上报的限流

这个示例每 200 ms 上报一次位置，全程 4 秒，也就是 20 条上行。
**真窗帘走 15 秒的话，200 ms 一条就是 75 条**——
对 ESP-NOW 来说太密了，会开始丢包。

真实现应该：

```c
#define REPORT_EVERY_N_STEPS 5

if (++s_step_count >= REPORT_EVERY_N_STEPS || s_cover.position == s_cover.target) {
    s_step_count = 0;
    en2m_report_cover_position(ENDPOINT, s_cover.position);
}
```

**到位的那一条必须发**（上面那个 `||`），中间的可以省。
组件本身也有一层最小上报间隔保护，见
[docs/reporting.md](../../docs/reporting.md)。

---

## 换成真硬件

### 直流电机 + H 桥（常见的电动窗帘电机）

```c
static void motor_drive(int dir)     // -1 开, 0 停, +1 关
{
    gpio_set_level(PIN_IN1, dir > 0);
    gpio_set_level(PIN_IN2, dir < 0);
}
```

**位置怎么知道？** 三种办法，可靠性递增：

| 办法 | 怎么做 | 问题 |
|---|---|---|
| **计时** | 记下全程秒数，按时间比例算位置 | 会漂，负载变化（窗帘重量、温度）就不准 |
| **霍尔 / 光电编码器** | 数电机转了多少圈 | 要接线，但准 |
| **限位开关 + 计时** | 两端有限位开关校准，中间靠计时 | **最实用的折中** |

限位开关的价值在于**每次走到端点都会重新对准**，
所以漂移不会累积。窗帘一天总会全开全关一次，误差自动清零。

### 交流管状电机（卷帘、遮阳篷）

管状电机通常是"三根线：开、关、零线"，内部自带限位。
用两个继电器控制开 / 关：

```c
static void motor_drive(int dir)
{
    /* 两个继电器绝对不能同时吸合 */
    gpio_set_level(PIN_RELAY_OPEN, 0);
    gpio_set_level(PIN_RELAY_CLOSE, 0);
    if (dir == 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));      // 等触点释放
    gpio_set_level(dir > 0 ? PIN_RELAY_CLOSE : PIN_RELAY_OPEN, 1);
}
```

**"先全断，等 100 ms，再合"这个顺序不能省。**
两个继电器同时吸合会把电机的两个绕组同时通电——轻则跳闸，重则烧电机。
更安全的做法是**硬件互锁**：把一个继电器的常闭触点串到另一个的线圈回路里，
这样物理上就不可能同时吸合。

### 堵转保护

真电机要加。窗帘撞到东西的时候电流会飙升：

- 串一个采样电阻 + `INA219` 或者 ADC 读电流
- 超过阈值就 `motor_stop()`
- 也可以只用超时：`if (行程时间 > 全程时间 * 1.5) motor_stop();`

超时保护最容易实现而且很有效，**至少要有这一个**。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| HA 里的窗帘方向反了 | 你在固件里也翻转了一次百分比。固件用 Matter 约定（0 = 全开），翻转交给 HA 集成 |
| 走到一半停不下来 | `STOP_MOTION` 没实现，或者 `motor_stop()` 里没重设 `target` |
| 位置越走越不准 | 纯计时方案的固有问题，加限位开关 |
| 行程中 HA 卡顿 / 丢位置 | 上报太密了，见"行程上报的限流" |
| 电机来回抽搐 | H 桥两路同时有效，或者继电器互锁没做 |
| 断电重启后位置是 0 但窗帘在一半 | 位置没持久化。可以把它设成 persisted，但更稳的是开机走到一个限位重新校准 |

---

## 延伸阅读

- [docs/examples.md#window_cover](../../docs/examples.md#window_cover) — 逐行精讲
- [docs/callbacks.md](../../docs/callbacks.md) — `command` 回调和 `write` 回调的语义差别
- [docs/reporting.md](../../docs/reporting.md) — 行程上报的限流机制

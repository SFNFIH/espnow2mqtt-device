# `fan_controller` — 风扇

**按 cluster 注册回调的示例。** 前面几个示例都用设备级的
`cfg.attribute_write`，一个回调管全设备；这里改成给 FanControl cluster
挂一个**专属**回调，还带自己的 `ctx`。

**一块板上有好几个不相干的外设时，这才是该用的模式。**

| | |
|---|---|
| Cluster | Fan Control (`0x0202`) |
| 设备类型 | `EN2M_DEVICE_TYPE_FAN` |
| 回调 | `en2m_cluster_set_write_cb()` 注册的 cluster 级 `write` |
| 驱动 | 无（示例里是内存 stub） |
| 默认名 / slug | `fan1` |
| HA 实体 | `fan.fan1` |

---

## 接线

**这个示例不接任何硬件。** `fan_apply()` 是一行 `ESP_LOGI`，
把当前的模式和占空比打到串口上。

调速风扇的驱动方式差别很大（PWM 直流、可控硅调压、EC 电机的 0–10 V），
所以示例里不预设一种。"换成真硬件"一节给了三种的写法。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/fan_controller
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

```
espnow2mqtt/fan1/availability online
espnow2mqtt/fan1/state          {"fan_mode":"off","percentage":0,"caps":["fan"],"hop":1}
```

手动控制：

```bash
mosquitto_pub -t espnow2mqtt/fan1/set -m '{"fan_mode":"medium"}'
mosquitto_pub -t espnow2mqtt/fan1/set -m '{"percentage":75}'
mosquitto_pub -t espnow2mqtt/fan1/set -m '{"fan_mode":"off"}'
```

串口：

```
I (9021) ex_fan: ceiling fan: mode=2 duty=50%
I (9412) ex_fan: ceiling fan: mode=3 duty=75%
```

`ceiling` 这个字符串来自 `ctx` 里带的 `fan_ctx_t`——见下文。

---

## 为什么要按 cluster 注册

对比两种写法：

```c
/* 设备级：一个回调，自己 switch cluster_id */
en2m_device_config_t cfg = { .attribute_write = on_write };

static esp_err_t on_write(const en2m_attr_path_t *path, ...) {
    switch (path->cluster_id) {
    case EN2M_CLUSTER_FAN_CONTROL:  ...
    case EN2M_CLUSTER_ON_OFF:       ...
    case EN2M_CLUSTER_THERMOSTAT:   ...
    }
}
```

```c
/* cluster 级：各管各的 */
fan_cluster = en2m_cluster_get(ep, EN2M_CLUSTER_FAN_CONTROL);
en2m_cluster_set_write_cb(fan_cluster, on_fan_write, &s_fan);
```

设备级适合"一个外设，几个相关 cluster"（比如
[`dimmable_light`](../dimmable_light) 的开关 + 亮度 + 色温，
它们都落到同一个灯驱动上）。

cluster 级适合"**几个互不相干的外设**"：

| 好处 | 说明 |
|---|---|
| **没有巨型 switch** | 加一个外设不用去动别人的回调 |
| **每个回调有自己的 `ctx`** | 不需要全局变量，也不需要在回调里查表找"这是哪个风扇" |
| **改一个不碰另一个** | 风扇的代码不会因为改温控器而重新编译出问题 |
| **可以复用同一个函数** | 两个风扇挂同一个 `on_fan_write`，靠 `ctx` 区分 |

最后那条是最实用的：

```c
static fan_ctx_t s_fan_a = {.label = "ceiling", .pin = GPIO_NUM_5};
static fan_ctx_t s_fan_b = {.label = "exhaust", .pin = GPIO_NUM_6};

en2m_cluster_set_write_cb(en2m_cluster_get(ep1, EN2M_CLUSTER_FAN_CONTROL), on_fan_write, &s_fan_a);
en2m_cluster_set_write_cb(en2m_cluster_get(ep2, EN2M_CLUSTER_FAN_CONTROL), on_fan_write, &s_fan_b);
```

**一个函数，两个风扇，零分支。** `ctx` 就是面向对象里的 `this`。

### 优先级

cluster 级的回调**优先于**设备级的。两个都注册时：

1. 先找这个 cluster 有没有自己的 write 回调 → 有就调它，**设备级不再调**
2. 没有 → 调设备级的 `cfg.attribute_write`

所以可以"设备级兜底 + 个别 cluster 特殊处理"。
详细规则见 [docs/callbacks.md](../../docs/callbacks.md)。

---

## mode 和 percent 的关系

FanControl cluster 有两个属性，它们说的是同一件事：

| 属性 | 类型 | 值 |
|---|---|---|
| `EN2M_ATTR_FAN_MODE` | 枚举 `en2m_fan_mode_t` | `EN2M_FAN_OFF` / `LOW` / `MEDIUM` / `HIGH` / `ON` / `AUTO` / `SMART`，上行序列化成 `off` / `low` / `medium` / `high` / `on` / `auto` / `smart` |
| `EN2M_ATTR_PERCENT_SETTING` | 0–100 | 百分比 |

这七档在 HA 里是 `fan` 实体的 **preset mode 下拉框**，百分比是滑块。
HA 拖滑块时会同时发 `percentage` 和一个 `fan_mode`（0 就是 `off`，其余是 `on`），
所以两个属性都会被写到。

**组件会自己保持这两个一致。** 你写 `fan_mode = high`，
组件把 `percent_setting` 也设成对应的值；反过来也一样。
所以 `on_fan_write()` 会被调**两次**（一次 mode、一次 percent），
`fan_apply()` 也跑两次。

这不是 bug，是 Matter 的模型如此：HA 的风扇界面既有"低/中/高"的档位下拉，
也有一个百分比滑块，两边都得能用。

**如果你的硬件只认百分比**（PWM 风扇），那就只处理 `PERCENT_SETTING`，
`FAN_MODE` 那个 case 直接 `break` 不做事——反正百分比那次调用会把活干了。

**如果你的硬件只有三档**（可控硅继电器切换），反过来：只处理 `FAN_MODE`。
注意这时候用户拖滑块拖到 37% 也会触发一次 mode 变化（映射成 `low`），
硬件仍然工作正常。

映射关系见 [docs/data-model.md](../../docs/data-model.md)。

---

## 换成真硬件

### 直流 PWM 风扇（12 V 4 线、机箱风扇）

```c
#include "driver/ledc.h"

static void fan_apply(fan_ctx_t *fan)
{
    /* 4 线风扇的 PWM 输入要 25 kHz，别用低频 */
    uint32_t duty = (uint32_t)fan->percent * 1023 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, fan->channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, fan->channel);
}
```

两个坑：

1. **PWM 频率**。4 线风扇的规范是 25 kHz。用几百 Hz 会听到明显的啸叫。
2. **最低启动占空比**。多数风扇低于 20–30% 转不起来（或者能转但启动不了）。
   在 `fan_apply()` 里做一个下限：`if (duty > 0 && duty < MIN_DUTY) duty = MIN_DUTY;`

4 线风扇还有一根**转速反馈（TACH）**线，接到 GPIO 上数脉冲就能读到实际转速，
可以额外挂一个 `EN2M_CLUSTER_ELECTRICAL_POWER` 之外的自定义属性上报。

### 交流吊扇（可控硅调压）

**不要用 PWM 调交流电压。** 正确做法是**过零检测 + 相位切割**，
需要一个过零信号输入和一个精确定时的触发输出。
现成的模块叫 "AC dimmer module"（RobotDyn 那种）。

真正的问题是**多数吊扇不能无级调速**：电机是抽头式的，
只能在几个固定绕组之间切换。这种就用三个继电器 + 只处理 `FAN_MODE`：

```c
static esp_err_t on_fan_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->attribute_id != EN2M_ATTR_FAN_MODE) {
        return ESP_OK;                      // percent 的那次调用直接放过
    }
    en2m_fan_mode_t mode = (en2m_fan_mode_t)value->v.e8;
    /* 先全断再合，避免两个绕组同时通电 */
    relay_all_off();
    vTaskDelay(pdMS_TO_TICKS(50));
    switch (mode) {
    case EN2M_FAN_LOW:    relay_on(PIN_LOW);  break;
    case EN2M_FAN_MEDIUM: relay_on(PIN_MED);  break;
    case EN2M_FAN_HIGH:   relay_on(PIN_HIGH); break;
    default: break;                          // off：全断就够了
    }
    return ESP_OK;
}
```

**"先全断再合"那两行不能省**：抽头电机两个绕组同时通电会烧。
那个 50 ms 是给继电器触点释放留的时间。

### EC 电机（0–10 V）

C3 没有 DAC，要用 PWM + RC 低通滤波，或者一个 I²C 的 DAC（MCP4725）。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| `fan_apply()` 每次被调两次 | 正常，见上面"mode 和 percent 的关系" |
| 风扇低速不转 | 没做最低启动占空比 |
| 风扇有啸叫 | PWM 频率太低，调到 25 kHz |
| HA 里滑块能拖但档位下拉没反应 | 你只处理了 `PERCENT_SETTING`。要档位就也处理 `FAN_MODE` |
| `en2m_cluster_get()` 返回 NULL | cluster 不在这个设备类型的配方里。要么换设备类型，要么 `en2m_cluster_create()` 手动加 |
| 拖滑块之后档位下拉变成了 `on` | 正常，HA 拖滑块时会连带发 `fan_mode: "on"` |

---

## 延伸阅读

- [docs/examples.md#fan_controller](../../docs/examples.md#fan_controller) — 逐行精讲
- [docs/callbacks.md](../../docs/callbacks.md) — cluster 级和设备级回调的优先级
- [docs/data-model.md](../../docs/data-model.md) — cluster / 属性 / endpoint 三层结构

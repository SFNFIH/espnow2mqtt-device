# `dimmable_light` — 可调光色温灯

**一个回调管三个 cluster 的示例。** 开关、亮度、色温在 Matter 的数据模型里
是三个不同的 cluster，但它们最后都落到同一个灯驱动上，
所以这里只写一个 `on_write()`，按 `path->cluster_id` 分发。

| | |
|---|---|
| Cluster | OnOff (`0x0006`) + Level Control (`0x0008`) + Color Control (`0x0300`) + Identify (`0x0003`) |
| 设备类型 | `EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT` |
| 回调 | `attribute_write` + `identify` |
| 驱动 | 无（示例里是内存 stub） |
| 默认名 / slug | `light1` |
| HA 实体 | `light.light1` |

---

## 接线

**这个示例不接任何硬件。** `light_apply()` 就是一行 `ESP_LOGI`，
把当前的开关 / 亮度 / 色温打到串口上。

这么写是故意的：你可以先把 HA → MQTT → ESP-NOW → 回调这条链路跑通，
确认亮度滑块推过来的数值是对的，再去接 LEDC 或者 LED 驱动 IC。
"换成真硬件"一节给了 LEDC 的完整写法。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/dimmable_light
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

MQTT（保留消息）：

```
espnow2mqtt/light1/availability online
espnow2mqtt/light1/state        {"switch":"OFF","brightness":254,"color_temp":300,
                                 "color_mode":"color_temp","caps":["light"],"hop":1}
```

在 HA 里拖亮度滑块，或者手动发：

```bash
mosquitto_pub -t espnow2mqtt/light1/set -m '{"switch":"ON"}'
mosquitto_pub -t espnow2mqtt/light1/set -m '{"brightness":128}'
mosquitto_pub -t espnow2mqtt/light1/set -m '{"color_temp":370}'
```

串口上每条都会打出一行完整状态：

```
I (12043) ex_light: output: on level=128 mireds=370
```

### `caps` 里为什么没有 `switch`

序列化的时候有一条特殊规则：**只要报了 `brightness` 或者 `color_temp`，
就把 `switch` 从 `caps` 里删掉**。

因为在 HA 里一个能调光的东西应该是 `light` 实体，不是 `switch` 实体。
两个都出的话你会在界面上看到两个控件控制同一盏灯，
而且自动化里挑错那个就没有亮度可调。

`state` 里的 `switch` 字段**还在**——`light` 实体靠它判断开关状态。
删掉的只是 `caps` 里的那一项。

---

## 数值范围

| 字段 | 范围 | 单位 | 说明 |
|---|---|---|---|
| `brightness` | 0–254 | — | **Matter 的范围，不是 0–255。** 254 是最亮 |
| `color_temp` | 154–500 | 迈尔德（mired） | 154 ≈ 6500 K 冷白，500 = 2000 K 暖黄 |

迈尔德和开尔文是**倒数关系**：`mired = 1000000 / kelvin`。
HA 的色温滑块两头显示的是开尔文（集成里限死 2000–6500 K），
走 MQTT 的是迈尔德，两次转换都在 HA 集成里做，固件只管迈尔德。

亮度也一样：HA 内部是 0–255，集成负责按 `× 254 / 255` 换成 Matter 的 0–254，
所以固件收到的永远是 0–254。照抄这个范围能省掉一层映射。
接 LEDC 的时候记得**自己缩放到占空比范围**，见下文。

---

## `identify` 回调

```c
static void on_identify(uint8_t endpoint_id, uint16_t seconds, void *ctx)
{
    ESP_LOGI(TAG, "identify endpoint %u, %u s left", endpoint_id, seconds);
}
```

Identify 是"让设备表明自己身份"——在一屋子同型号的灯里找出哪一个是
`light1`。真实现应该在这几秒里**闪灯**：

```c
static void on_identify(uint8_t endpoint_id, uint16_t seconds, void *ctx)
{
    if (seconds == 0) {
        blink_stop();              // 0 表示提前取消
        return;
    }
    blink_start(seconds);
}
```

注意 Identify cluster 是**手动加的**：

```c
ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT);
en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);
```

设备类型的"配方"里只有必需 cluster，可选的自己补。
这个模式对任何可选 cluster 都适用。

---

## 换成真硬件

### 单色 / 双色温 LED 条（LEDC）

```c
#include "driver/ledc.h"

#define CH_WARM LEDC_CHANNEL_0
#define CH_COLD LEDC_CHANNEL_1
#define DUTY_MAX 8191              // 13 位分辨率

static void light_apply(void)
{
    uint32_t total = s_light.on ? (uint32_t)s_light.level * DUTY_MAX / 254 : 0;

    /* 色温 = 两路的配比。153 全冷，500 全暖。 */
    uint32_t warm_pct = (s_light.mireds - 153) * 100 / (500 - 153);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_WARM, total * warm_pct / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_COLD, total * (100 - warm_pct) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_WARM);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_COLD);
}
```

三件事容易忘：

1. **PWM 频率要高于 1 kHz**，否则手机摄像头拍过去有频闪条纹，肉眼在余光里也能看到。
   示例用 5 kHz 比较稳。
2. **低亮度端要做 gamma 校正。** 占空比和人眼感受不是线性的，
   线性映射的话 `brightness` 从 1 到 20 看起来几乎没变化，到 200 之后又几乎一样亮。
   查表或者用 `duty = DUTY_MAX * (level/254)^2.2`。
3. **`light_apply()` 跑在 en2m 任务上**，所以别在里面做渐变循环。
   要渐变就起一个 `esp_timer`，在定时器里推进。

### 换 WS2812 / SK6812

`led_strip` 组件（`idf.py add-dependency espressif/led_strip`）可以直接用。
色温转 RGB 需要一张色温表，或者用 `led_strip_set_pixel_rgbw` 的 W 通道。

要支持真彩色的话还得加 `EN2M_ATTR_CURRENT_HUE` / `CURRENT_SATURATION`，
数据模型侧的改法见 [docs/data-model.md](../../docs/data-model.md)。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| HA 里出的是 `switch` 不是 `light` | 设备还没报过 `brightness`。第一条上报之后就会变成 `light` |
| 亮度只有 0 和 254 两档 | 你的 `light_apply()` 只看了 `s_light.on` |
| 拖滑块灯闪一下才到位 | 渐变写在回调里阻塞了，改用 `esp_timer` |
| 色温反了（冷暖颠倒） | 迈尔德和开尔文是倒数，别把两边搞混 |
| HA 里色温滑块是灰的 | `color_mode` 字段没报出去。序列化时它和 `color_temp` 一起发，缺了说明 Color Control cluster 没建 |

---

## 延伸阅读

- [docs/examples.md#dimmable_light](../../docs/examples.md#dimmable_light) — 逐行精讲
- [docs/data-model.md](../../docs/data-model.md) — cluster / 属性 / endpoint 三层结构
- [docs/callbacks.md](../../docs/callbacks.md) — 一个回调分发多 cluster 的推荐写法

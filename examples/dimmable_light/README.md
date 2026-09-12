# `dimmable_light` — 可调光色温灯

**一个回调管三个 cluster 的示例。** 开关、亮度、色温在 Matter 的数据模型里
是三个不同的 cluster，但它们最后都落到同一个灯驱动上，
所以这里只写一个 `on_write()`，按 `path->cluster_id` 分发。

| | |
|---|---|
| Cluster | OnOff (`0x0006`) + Level Control (`0x0008`) + Color Control (`0x0300`) + Identify (`0x0003`) |
| 设备类型 | `EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT` |
| 回调 | `attribute_write` + `identify` |
| 外设组件 | [`espressif/led_strip`](https://components.espressif.com/components/espressif/led_strip) `^3.0.3`（RMT 后端） |
| 默认名 / slug | `light1` |
| HA 实体 | `light.light1` |

---

## 接线

**默认配置不用接任何线。** `PIN_STRIP` 是 `GPIO_NUM_8`，
也就是 ESP32-C3-DevKitM-1 和 DevKitC-02 上**板载的那颗 WS2812**，
烧进去就能看到它亮起来。

要接外接灯带：

| GPIO | 接什么 | 说明 |
|---|---|---|
| **8** | 灯带 `DIN` | 串一个 220–470 Ω 电阻，抑制反射 |
| 5 V | 灯带 `VCC` | **不要从开发板的 5 V 取电**，见下面 |
| GND | 灯带 `GND` | 必须和 C3 共地 |

改 `main/main.c` 顶部两行就换引脚和灯珠数量：

```c
#define PIN_STRIP GPIO_NUM_8
#define STRIP_LEDS 1
```

`STRIP_LEDS` 改成多少，整条灯带就同色一起变。
换引脚前先看 [docs/wiring.md](../../docs/wiring.md#esp32-c3-引脚选择须知)。

> **供电是 WS2812 最常见的坑。** 一颗灯珠满白约 60 mA，
> 30 颗就是 1.8 A —— 远超开发板 USB 口能给的。
> 超过 8 颗就该用独立 5 V 电源，只把 GND 和数据线接回 C3。
> 电源不够的典型症状是灯带尾部发红、C3 反复重启。

> **3.3 V 电平驱动 5 V 灯带**在短线上一般能用，长线或者整条不亮的话
> 需要一片电平转换（74AHCT125 之类）。
> WS2812B 的数据线阈值是 `0.7 × VDD`，也就是 3.5 V，C3 的 3.3 V 是踩在边上的。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/dimmable_light
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

第一次 `build` 会联网把 `espressif/led_strip` 下载到本工程的
`managed_components/`，不需要手动装。依赖写在
[`main/idf_component.yml`](main/idf_component.yml) 里。

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
I (12043) ex_light: output: on level=128 mireds=370 rgb=64,48,33
```

`rgb=` 是色温和亮度一起算完之后**真正推给灯珠**的值，
调不出想要的颜色时先看这一行，就知道是数据模型侧的问题还是转换的问题。

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

## 色温怎么变成 RGB

WS2812 的灯珠是 RGB 三色，而 Matter 的数据模型说的是**迈尔德**。
中间那一步转换是这个示例唯一真正有内容的代码：

```c
static const struct { uint16_t kelvin; uint8_t r, g, b; } s_blackbody[] = {
    {2000, 255, 141, 11},  {2500, 255, 165, 71},  {3000, 255, 180, 107},
    ...
    {6500, 255, 249, 253},
};
```

这是一张**黑体辐射表**，从 2000 K 到 6500 K 每 500 K 一个点，
中间线性插值。范围挑得和 HA 的色温滑块（154–500 迈尔德）完全对上，
所以滑块推到两头都有对应的颜色，不会截断。

注意红色分量**全程都是 255**，只有绿和蓝在动。
这不是表做得糙——真实的黑体在这个温度区间里就是这样，
"暖"就是把绿蓝压下去，"冷"就是把它们补上来。

### gamma 只加在亮度上

```c
gamma_level = (uint32_t)s_light.level * s_light.level / 254u;
r = (uint8_t)((uint32_t)r * gamma_level / 254u);
g = (uint8_t)((uint32_t)g * gamma_level / 254u);
b = (uint8_t)((uint32_t)b * gamma_level / 254u);
```

WS2812 的占空比是线性的，人眼不是。直接拿 `level` 去乘三个通道，
结果是滑块下面一小段看着全黑、上面一大段看着一样亮。
先把亮度平方（γ≈2.0）再乘，感知上的步进就均匀了。

**关键是 gamma 加在亮度上，而不是分别加在 R/G/B 上。**
如果三个通道各自做平方，通道之间的**比例**会变
（255² 和 141² 的比不等于 255 和 141 的比），色温就跟着亮度飘了：
调暗一点颜色就偏冷。先算好比例、再整体缩放，色温才稳。

---

## 换成别的灯

### 单色 / 双色温 LED 条（LEDC）

没有 WS2812、只有一路或两路普通 LED 的时候，把
`main/idf_component.yml` 里的 `espressif/led_strip` 删掉，
改用 IDF 内置的 LEDC：

```c
#include "driver/ledc.h"

#define CH_WARM LEDC_CHANNEL_0
#define CH_COLD LEDC_CHANNEL_1
#define DUTY_MAX 8191              // 13 位分辨率

static void light_apply(void)
{
    uint32_t total = s_light.on ? (uint32_t)s_light.level * s_light.level * DUTY_MAX
                                      / (254 * 254)
                                : 0;

    /* 色温 = 两路的配比。154 全冷，500 全暖。 */
    uint32_t warm_pct = (s_light.mireds - 154) * 100 / (500 - 154);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_WARM, total * warm_pct / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_COLD, total * (100 - warm_pct) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_WARM);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_COLD);
}
```

两件事容易忘：

1. **PWM 频率要高于 1 kHz**，否则手机摄像头拍过去有频闪条纹，
   肉眼在余光里也能看到。5 kHz 比较稳。
2. **gamma 校正一样要做**，理由和上面那一节完全相同。

### 支持真彩色

现在这个示例是 `COLOR_TEMPERATURE_LIGHT`，HA 里只给亮度和色温两个滑块。
要出色盘就得加 `EN2M_ATTR_CURRENT_HUE` / `CURRENT_SATURATION`，
设备类型换成 `EN2M_DEVICE_TYPE_EXTENDED_COLOR_LIGHT`。
硬件侧不用改——`led_strip_set_pixel_hsv()` 直接吃 HSV，
把 `mireds_to_rgb()` 整段删掉就行。
数据模型侧的改法见 [docs/data-model.md](../../docs/data-model.md)。

### 逐颗寻址

示例里整条灯带同色，因为 Matter 的 Color Control 描述的是"一盏灯"。
要做流水灯、渐变这类效果，`light_apply()` 里的 `for` 循环改成按 `i` 算颜色即可。
但**别在 `light_apply()` 里跑动画循环**——它跑在 en2m 任务上，
一阻塞整个网络就停了。起一个 `esp_timer`，在定时器回调里推进一帧。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| HA 里出的是 `switch` 不是 `light` | 设备还没报过 `brightness`。第一条上报之后就会变成 `light` |
| 亮度只有 0 和 254 两档 | 你的 `light_apply()` 只看了 `s_light.on` |
| 拖滑块灯闪一下才到位 | 渐变写在回调里阻塞了，改用 `esp_timer` |
| 色温反了（冷暖颠倒） | 迈尔德和开尔文是倒数，别把两边搞混 |
| 灯带整条不亮，串口正常 | 十有八九是数据线电平不够或者没共地 |
| 只亮第一颗 | `STRIP_LEDS` 还是默认的 `1` |
| 颜色对不上（红绿互换） | 灯珠的通道顺序不是 GRB。改 `.color_component_format`，SK6812 常见是 `LED_STRIP_COLOR_COMPONENT_FMT_RGB` |
| 尾部灯珠发红 / 一亮就重启 | 供电不够，见上面接线那一段 |
| 调暗之后颜色偏冷 | 你把 gamma 分别加到 R/G/B 上了，见上文 |
| HA 里色温滑块是灰的 | `color_mode` 字段没报出去。序列化时它和 `color_temp` 一起发，缺了说明 Color Control cluster 没建 |

---

## 延伸阅读

- [docs/examples.md#dimmable_light](../../docs/examples.md#dimmable_light) — 逐行精讲
- [docs/data-model.md](../../docs/data-model.md) — cluster / 属性 / endpoint 三层结构
- [docs/callbacks.md](../../docs/callbacks.md) — 一个回调分发多 cluster 的推荐写法

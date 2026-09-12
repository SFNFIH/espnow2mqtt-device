# 示例

每个子目录都是一个**可以独立编译烧写的 ESP-IDF 工程**，目标芯片 ESP32-C3。
共享的组件在仓库根目录的 [`components/en2m`](../components/en2m)。

**示例里所有外设驱动都来自 [ESP 组件注册表](https://components.espressif.com)**，
本仓库不自带任何手写驱动。每个工程的 `main/idf_component.yml` 声明自己要哪几个组件，
`idf.py build` 会自动下载到该工程的 `managed_components/`，你不需要手动装什么。

先读这一句再选示例：**这十一个示例不是十一种设备，是十一种"把硬件接到这个库上"的姿势。**
你要抄的是**回调组合**，不是设备类型。想做一个水浸传感器，
设备类型在这里没有对应示例，但它的回调组合和 `contact_sensor` 一模一样，抄它就对了。

---

## 选型表

| 示例 | 做什么 | 回调组合 | 接的外设 | HA 里出现 |
|---|---|---|---|---|
| [`relay_switch`](relay_switch) | 继电器开关 | `write` + `changed` | 继电器 + 按键 | `switch` |
| [`dimmable_light`](dimmable_light) | 色温灯 | `write` + `identify` | WS2812 灯带 | `light` |
| [`smart_plug`](smart_plug) | 计量插座 | `write` + `read` | 继电器 + 按键 | `switch` + 2 个 `sensor` |
| [`th_sensor`](th_sensor) | 温湿度 | 只有 `read` | AHT20 | 2 个 `sensor` |
| [`contact_sensor`](contact_sensor) | 门磁 | `read` + 边沿推送 | 干簧管 | `binary_sensor` |
| [`scene_switch`](scene_switch) | 无线按键 / 场景开关 | 一个都没有 | 按键 | `event` |
| [`occupancy_sensor`](occupancy_sensor) | 人在 + 照度 | `read` + `en2m_schedule` | PIR + BH1750 | `binary_sensor` + `sensor` |
| [`fan_controller`](fan_controller) | 风扇 | 按 cluster 注册的 `write` | 无（stub） | `fan` |
| [`window_cover`](window_cover) | 窗帘 / 卷帘 | 只有 `command` | 无（stub） | `cover` |
| [`door_lock`](door_lock) | 门锁 | 只有 `write` | 无（stub） | `lock` |
| [`thermostat`](thermostat) | 温控器 | `write` + `read` + `changed` | 无（stub） | `climate` |
| [`../firmware/router`](../firmware/router) | 中继 | 只有事件 | 只要供电 | 不出实体 |

"stub" 的意思是这个示例把执行器写成了几行 `ESP_LOGI`，
方便你**先把链路跑通再接硬件**。每个 README 的"换成真硬件"一节告诉你要改哪几行。

前七个示例接的是真外设，后面四个（风扇、窗帘、门锁、温控器）是 stub —— 
不是偷懒，是这四类的执行器差异太大（步进电机、舵机、电磁锁、继电器 + PID），
写死任何一种都不如留一个明确的"在这里接你的硬件"位置。

### 外设组件

| 外设 | 组件 | 版本 | 用在哪 |
|---|---|---|---|
| WS2812 / SK6812 灯带 | [`espressif/led_strip`](https://components.espressif.com/components/espressif/led_strip) | `^3.0.3` | `dimmable_light` |
| 按键（单击/双击/长按/释放） | [`espressif/button`](https://components.espressif.com/components/espressif/button) | `^4.2.1` | `relay_switch`、`smart_plug`、`scene_switch` |
| 干簧管 / 门磁 | 同上 | `^4.2.1` | `contact_sensor` |
| PIR 人体感应 | 同上 | `^4.2.1` | `occupancy_sensor` |
| AHT20 温湿度 | [`espressif/aht20`](https://components.espressif.com/components/espressif/aht20) | `^2.0.0` | `th_sensor` |
| BH1750 照度 | [`espressif/bh1750`](https://components.espressif.com/components/espressif/bh1750) | `^2.0.0` | `occupancy_sensor` |
| 继电器 | ESP-IDF 内置 `driver`（一路 GPIO 输出） | — | `relay_switch`、`smart_plug` |

几件值得先知道的事：

- **干簧管、PIR 和按键用的是同一个组件。** 这三样在电路上是同一种东西：
  一根线，两个电平。`espressif/button` 已经替你做好消抖和手势识别，
  再写一遍 GPIO 中断没有意义。
- **继电器没有组件**，因为没有东西可抽象：一个引脚，一个电平，
  `gpio_set_level()` 就是全部。注册表里没有它不是遗漏。
- **注册表里没有 DHT11/DHT22 组件**，所以 `th_sensor` 用的是 AHT20。
  AHT20 是 DHT22 的现代 I²C 替代品：同样的量程和精度，
  不需要自己掐单总线时序，而且驱动由乐鑫维护。
- **AHT20 和 BH1750 用的是两套 I²C 驱动**：`aht20` 走
  [`espressif/i2c_bus`](https://components.espressif.com/components/espressif/i2c_bus)
  封装，`bh1750` 直接用 IDF 5.x 的 `driver/i2c_master.h`。
  两者不能共用一个 port 句柄。这两个示例各自独立，所以不冲突，
  但你要是想把它们合到一块板上，得先把其中一个换掉。

**十一个示例里没有一个 `while (1)`。** 这是这套库的设计目标：
应用代码只写回调，任务归组件。

### 怎么挑

- **第一次读这个仓库** → [`relay_switch`](relay_switch)。112 行，把核心思想全讲完了。
- **我要做传感器** → 采样很便宜（I²C 读一下）就抄 [`th_sensor`](th_sensor)（只有 `read`）；
  状态是中断来的（门磁、PIR、水浸）就抄 [`contact_sensor`](contact_sensor)。
- **我要做执行器** → 能瞬间到位（继电器、PWM）就抄 [`relay_switch`](relay_switch)（`write`）；
  要走一段时间（窗帘、阀门）就抄 [`window_cover`](window_cover)（`command`）。
- **我要做电池设备** → 抄 [`scene_switch`](scene_switch)，它的 `EN2M_REPORT_ON_CHANGE_ONLY`
  让射频只在真有事的时候醒。
- **一块板上好几个不相干的外设** → 抄 [`fan_controller`](fan_controller) 的按 cluster 注册回调。

---

## 通用编译流程

```bash
. ~/esp/esp-idf-v5.5.5/export.sh      # 你自己的 ESP-IDF 路径
cd examples/relay_switch
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

每个工程的 `CMakeLists.txt` 里已经写好 `EXTRA_COMPONENT_DIRS` 指向 `../../components`，
不需要额外配置。`sdkconfig.defaults` 里已经设好目标芯片、4 MB flash、
USB-Serial-JTAG 控制台和 `FREERTOS_HZ=1000`。

**唯一一个你几乎肯定要改的配置是信道**：

```bash
echo 'CONFIG_EN2M_WIFI_CHANNEL=6' >> sdkconfig.defaults
idf.py fullclean && idf.py build
```

**全网（协调器 + 路由器 + 所有设备）的信道必须一致。**
不一致时完全静默：没有错误、没有日志，设备就是找不到父节点。
这是"死活不上线"的头号原因。其它配置项见 [docs/kconfig.md](../docs/kconfig.md)。

---

## 通用验证流程

不管烧的是哪个示例，上线过程都是同一套。

### 1. 串口上等 `parent=`

```
I (612)  en2m:      mesh init
I (615)  ex_switch: ready — the component owns the task from here
I (1843) en2m:      parent=aa:bb:cc:dd:ee:ff cost=1 rssi=-42
```

**`parent=` 这一行才叫入网成功**，前两行只说明固件跑起来了。
十秒内不出现就是信道对不上，见
[docs/troubleshooting.md 第 1 节](../docs/troubleshooting.md#1-设备完全不上线)。

### 2. 开配网窗口

设备第一次上线要让协调器"认识"一次，不然协调器会丢掉陌生设备的上行
（Bridge 日志里是 `drop uplink (not pairing / unknown)`）：

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60
```

然后 60 秒内复位一下 C3。

### 3. 看 MQTT

```bash
mosquitto_sub -t 'espnow2mqtt/#' -v
```

设备上线后应该有两条**保留消息**：

```
espnow2mqtt/relay1/availability online
espnow2mqtt/relay1/state        {"switch":"OFF","caps":["switch"],"hop":1,"via":"..."}
```

`relay1` 这个 slug 来自 `main.c` 里 `cfg.mesh.name`，小写、空格换下划线。
没设 `name` 的话用去掉冒号的 MAC。

`hop`、`via`、`caps` 是 Bridge 加上去的，设备自己不发这几个字段。
`caps` 决定 HA 出什么实体，各示例的 README 里都写了自己那份。

### 4. 在 HA 里确认

HA 集成装好之后（[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)），
设备会自己出现在"设备与服务"里，不用手写 YAML。
实体名是 `<平台>.<slug>_<能力>`，比如 `switch.relay1`、`sensor.th_sensor1_temperature`。

除了上表里那些**功能实体**，每个节点还会多出三个**诊断实体**
（在设备页的"诊断"折叠区里，不占主界面）：

| 实体 | 来自 | 有什么用 |
|---|---|---|
| `sensor.<slug>_mesh_hop` | Bridge 加的 `hop` | 到协调器几跳。>1 说明走了路由器 |
| `sensor.<slug>_rssi` | 链路统计 | 信号余量。差于 −85 dBm 就该挪位置了 |
| `sensor.<slug>_node_role` | `node_role` | `leaf` / `router` / `coordinator` |

这三个不需要设备声明任何 cap，所有节点都有。

---

## 目录结构

一个示例只有三个文件（外加一个 `sdkconfig.defaults`）：

```
examples/relay_switch/
├── CMakeLists.txt          # 工程入口，指定 EXTRA_COMPONENT_DIRS
├── sdkconfig.defaults      # 目标芯片、flash、控制台
├── README.md
└── main/
    ├── CMakeLists.txt      # 组件注册
    ├── idf_component.yml   # 要从注册表拉哪些外设组件
    └── main.c              # 全部应用代码
```

要新建一个自己的设备，**直接 `cp -r` 一个最像的示例**，改 `project()` 名字和 `main.c` 就行。
要换外设就改 `main/idf_component.yml`：在
[components.espressif.com](https://components.espressif.com) 上找到组件，
把它的名字和版本写进 `dependencies`，下次 `idf.py build` 自动下载。

---

## 延伸阅读

- [docs/examples.md](../docs/examples.md) — 同样十一个示例的**逐行精讲**，
  解释每一处"为什么这么写"，以及换真硬件时的陷阱。这里的 README 只管"怎么跑起来"。
- [docs/quickstart.md](../docs/quickstart.md) — 从零到 HA 里能点亮的完整七步。
- [docs/callbacks.md](../docs/callbacks.md) — 五种回调各自的语义、线程和返回值约定。
- [docs/wiring.md](../docs/wiring.md) — C3 引脚哪些能用哪些不能用、供电和天线。
- [docs/api-reference.md](../docs/api-reference.md) — 全部公开 API。

# 示例

每个子目录都是一个**可以独立编译烧写的 ESP-IDF 工程**，目标芯片 ESP32-C3。
共享的组件在仓库根目录的 [`components/en2m`](../components/en2m)，
共享的参考驱动在 [`drivers/`](../drivers)。

先读这一句再选示例：**这十一个示例不是十一种设备，是十一种"把硬件接到这个库上"的姿势。**
你要抄的是**回调组合**，不是设备类型。想做一个水浸传感器，
设备类型在这里没有对应示例，但它的回调组合和 `contact_sensor` 一模一样，抄它就对了。

---

## 选型表

| 示例 | 做什么 | 回调组合 | 接的外设 | HA 里出现 |
|---|---|---|---|---|
| [`relay_switch`](relay_switch) | 继电器开关 | `write` + `changed` | 继电器 + 按键 | `switch` |
| [`dimmable_light`](dimmable_light) | 色温灯 | `write` + `identify` | 无（stub） | `light` |
| [`smart_plug`](smart_plug) | 计量插座 | `write` + `read` | 继电器 + 按键 | `switch` + 2 个 `sensor` |
| [`th_sensor`](th_sensor) | 温湿度 | 只有 `read` | DHT22 | 2 个 `sensor` |
| [`contact_sensor`](contact_sensor) | 门磁 | `read` + 中断推送 | 干簧管 | `binary_sensor` |
| [`scene_switch`](scene_switch) | 无线按键 / 场景开关 | 一个都没有 | 按键 | `event` |
| [`occupancy_sensor`](occupancy_sensor) | 人在 + 照度 | `read` + `en2m_schedule` | 无（stub） | `binary_sensor` + `sensor` |
| [`fan_controller`](fan_controller) | 风扇 | 按 cluster 注册的 `write` | 无（stub） | `fan` |
| [`window_cover`](window_cover) | 窗帘 / 卷帘 | 只有 `command` | 无（stub） | `cover` |
| [`door_lock`](door_lock) | 门锁 | 只有 `write` | 无（stub） | `lock` |
| [`thermostat`](thermostat) | 温控器 | `write` + `read` + `changed` | 无（stub） | `climate` |
| [`../firmware/router`](../firmware/router) | 中继 | 只有事件 | 只要供电 | 不出实体 |

"stub" 的意思是这个示例把执行器写成了几行 `ESP_LOGI`，
方便你**先把链路跑通再接硬件**。每个 README 的"换成真硬件"一节告诉你要改哪几行。

**十一个示例里没有一个 `while (1)`。** 这是这套库的设计目标：
应用代码只写回调，任务归组件。

### 怎么挑

- **第一次读这个仓库** → [`relay_switch`](relay_switch)。79 行，把核心思想全讲完了。
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
    ├── CMakeLists.txt      # 组件注册，列出用到的 drivers/*.c
    └── main.c              # 全部应用代码
```

要新建一个自己的设备，**直接 `cp -r` 一个最像的示例**，改 `project()` 名字和 `main.c` 就行。

---

## 延伸阅读

- [docs/examples.md](../docs/examples.md) — 同样十一个示例的**逐行精讲**，
  解释每一处"为什么这么写"，以及换真硬件时的陷阱。这里的 README 只管"怎么跑起来"。
- [docs/quickstart.md](../docs/quickstart.md) — 从零到 HA 里能点亮的完整七步。
- [docs/callbacks.md](../docs/callbacks.md) — 五种回调各自的语义、线程和返回值约定。
- [docs/wiring.md](../docs/wiring.md) — C3 引脚哪些能用哪些不能用、供电和天线。
- [docs/api-reference.md](../docs/api-reference.md) — 全部公开 API。

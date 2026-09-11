# ESP-NOW 2 MQTT — 设备（ESP32-C3）

**本仓库 = C3（及同类）终端 / 路由设备固件。**

- ESP-IDF 组件 **`en2m`**：mesh + Matter 风格 Endpoint/Cluster/Attribute，**回调驱动，自带任务**
- **`examples/*`**：开关、灯、风扇、窗帘、锁、温控、传感等 10 个示例（仓库根目录）
- **`drivers/`**：参考驱动（GPIO / 按键 / 门磁 / DHT），**不属于** `en2m`
- **`firmware/router`**：常电转发节点

S3 主机（协调器 + Bridge）在：**[espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host)**  
HA 插件在：**[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)**  
总览：**[espnow2mqtt](https://github.com/SFNFIH/espnow2mqtt)**

```
你的硬件驱动  ←回调─  en2m model  ──►  en2m mesh  --ESP-NOW-->  S3 协调器
```

---

## 和 ESP-Matter 一样的分层

| 层 | 在哪 |
|----|------|
| 驱动（GPIO/I2C/DHT/PWM…） | 你的 `app_main` / `drivers/` |
| 交互（Endpoint / Cluster / Attribute / 命令 / 上报） | `components/en2m` → `en2m_model` |
| 传输（ESP-NOW 树、重传） | `components/en2m` → `en2m_mesh` |

**`en2m` 不碰硬件，但它持有状态、任务和时序。**
组件里有一个自己的任务，负责 ESP-NOW 收包、mesh 维护、下行重传、传感器采样和上报。
**应用不需要轮询，通常也不需要自己建任务。**

---

## 一个完整设备

```c
#include "en2m.h"

/* 整个固件里唯一碰硬件的地方 */
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
        return relay_set(value->v.b);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "my-sw"},
        .attribute_write = on_write,
    };

    en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
    ESP_ERROR_CHECK(en2m_start(&cfg));
}
```

没有 `while` 循环，没有上报代码，也没有状态变量：继电器状态存在属性表里，
自动持久化到 NVS，开机自动写回驱动，变化时自动上报。

### 两条访问路径（务必区分）

| | 谁是真相来源 | 行为 |
|---|---|---|
| `en2m_attribute_set` | 应用 | 直接写入并上报。**传感器用这个**（或 `en2m_report_*` 包装） |
| `en2m_attribute_write` | 网络下发 / 本地控制 | 先调 write 回调，回调成功才写入并上报。**执行器用这个** |

所以物理按键也走 `en2m_attribute_write`，和 Home Assistant 下发走完全相同的路径。

### 回调

五个回调全部可选，全部在 en2m 任务上执行，既能按设备设置
（`en2m_device_config_t`），也能按 cluster 设置（`en2m_cluster_set_write_cb` 等）。

| 回调 | 用途 |
|---|---|
| `attribute_write` | 把值落到硬件。返回非 `ESP_OK` 则**不写入、不上报** |
| `attribute_read` | 上报前采样拉取型传感器（I2C / 单总线 / ADC） |
| `attribute_changed` | 观察已提交的变化（日志、屏幕、本地联动） |
| `command` | 属性模型表达不了的命令，比如窗帘 `stop` |
| `identify` | Identify 倒计时期间闪灯 / 蜂鸣 |

### 中断里怎么办

```c
static void toggle(void *arg)          /* 在 en2m 任务上执行，可用全部 API */
{
    en2m_value_t on;
    en2m_attribute_get(1, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, &on);
    en2m_attribute_write(1, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!on.v.b));
}

static void button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    en2m_schedule_from_isr(toggle, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}
```

只需要发一个值时用 `en2m_attribute_set_from_isr()`。

### 事件

所有异步行为同时发布在 ESP-IDF 默认事件循环上：

```c
en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m, NULL);
```

`STARTED`、`STOPPED`、`PARENT_FOUND`、`PARENT_LOST`、`PAIRING_CHANGED`、
`ATTRIBUTE_UPDATED`、`COMMAND_RECEIVED`、`REPORT_SENT`、`IDENTIFY`、
`ACK_RECEIVED`、`ACK_TIMEOUT`、`RX_DROPPED`。

完整 API 见 [`components/en2m/README.md`](components/en2m/README.md)。

---

## 快速烧录（C3）

需要 [ESP-IDF 5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/)（已在 5.5.5 上验证）。

```bash
cd examples/relay_switch   # 或其它示例
idf.py set-target esp32c3
idf.py build flash monitor
```

改 `main.c` 里的 `.name` / `.model`，配对前先在主机侧 `permit_join`。

路由节点：

```bash
cd firmware/router
idf.py set-target esp32c3
idf.py build flash
```

---

## 示例一览

每个示例都没有应用循环，只有回调。

| 目录 | 能力 | 演示的是什么 | HA 实体 |
|------|------|--------------|---------|
| `relay_switch` | OnOff | write 回调 + 按键中断 `en2m_schedule_from_isr` | Switch |
| `th_sensor` | 温湿度 | 纯 read 回调，按需采样 DHT | Sensor |
| `contact_sensor` | 门磁 | GPIO 中断推送 + read 回调兜底 | Binary sensor |
| `smart_plug` | OnOff + 功率/电量 | write 与 read 混用 | Switch + Sensor |
| `dimmable_light` | 亮度 + 色温 | 一个 write 回调覆盖三个 cluster，含 Identify | Light |
| `fan_controller` | 风扇 | **按 cluster** 注册回调并带自己的 ctx | Fan |
| `window_cover` | 窗帘 | command 回调 + 行程中持续上报真实位置 | Cover |
| `door_lock` | 门锁 | 持久化状态开机回写执行器 | Lock |
| `thermostat` | 温控 | write + read + changed，本地控温闭环 | Climate |
| `occupancy_sensor` | 人体 + 光照 | `esp_timer` 事件源 + `en2m_schedule` | Binary + Sensor |

部分示例的硬件部分是 **内存 stub**，先打通 MQTT/HA，再换成真实驱动。

---

## Cluster 与 HA 能力

| Cluster | HA `caps` |
|---------|-----------|
| OnOff | `switch` |
| LevelControl / ColorControl | `light` |
| FanControl / WindowCovering / DoorLock / Thermostat | `fan` / `cover` / `lock` / `climate` |
| Temperature / Humidity / Pressure / Illuminance / ElectricalPower | 对应 sensor |
| BooleanState / Occupancy / SmokeCO | binary_sensor |
| Identify | — |

上报 JSON ≤ `EN2M_DATA_MAX`（160 字节），扁平字段 + `caps`；放不下时组件按
「诊断字段 → `caps`」的顺序丢弃可选内容，而不是截断成坏 JSON。主机侧会合并
历次上报，所以部分上报不会丢信息。

---

## 从 driver-ops 旧 API 迁移

0.4 之前需要填 `en2m_on_off_driver_t` 之类的结构体，并在自己的循环里调
`en2m_model_loop()`。这些结构体已经删除：

| 旧写法 | 新写法 |
|---|---|
| `en2m_endpoint_add_on_off(ep, &drv)` | `en2m_cluster_create(ep, EN2M_CLUSTER_ON_OFF)` + `attribute_write` 回调 |
| `driver.set(on, ctx)` | `attribute_write` |
| `driver.get(&on, ctx)` | 不需要，组件自己缓存；只有实时传感器才用 `attribute_read` |
| `en2m_model_start(&mesh)` | `en2m_start(&device_config)` |
| `while (1) { en2m_model_loop(); }` | 删掉，两个 loop 现在都是空函数 |
| `en2m_model_notify(...)` | `en2m_attribute_set()`，它自己会上报 |

---

## 仓库结构

```
components/en2m/              # 核心组件（库）
examples/                     # 10 个示例工程
drivers/                      # 参考驱动
firmware/router/              # 常电路由
docs/
protocol/PROTOCOL.md
```

---

## 文档

**[docs/](docs/README.md) 是完整文档的入口**，按"我想做什么"分好了路线。
下面是最常用的几篇：

| 我想…… | 看这篇 |
|---|---|
| 先跑起来，看到 HA 里出实体 | [docs/quickstart.md](docs/quickstart.md) |
| **从零写一个自己的设备** | [docs/usage.md](docs/usage.md) |
| 照着现成示例改 | [docs/examples.md](docs/examples.md) |
| 搞懂整体架构和分层取舍 | [docs/architecture.md](docs/architecture.md) |
| **搞懂状态怎么流转**（启动、上报、命令、入网） | [docs/state-flow.md](docs/state-flow.md) |
| 搞懂五个回调的完整契约 | [docs/callbacks.md](docs/callbacks.md) |
| 查某个函数的准确语义和可调用上下文 | [docs/api-reference.md](docs/api-reference.md) |
| 全部 cluster / attribute ID 和设备类型 | [docs/data-model.md](docs/data-model.md) |
| 调上报频率、看 HA JSON 键和单位 | [docs/reporting.md](docs/reporting.md) |
| 搞懂 mesh 组网、选父、配网 | [docs/mesh.md](docs/mesh.md) |
| 调内存 / 超时 / 信道 | [docs/kconfig.md](docs/kconfig.md) |
| **设备不上线 / 命令不生效 / 状态不更新** | [docs/troubleshooting.md](docs/troubleshooting.md) |
| 从旧的 driver-ops 版本升级 | [docs/migration.md](docs/migration.md) |
| 看空中协议和 USB 协议 | [protocol/PROTOCOL.md](protocol/PROTOCOL.md) |
| 看英文 API 速查 | [components/en2m/README.md](components/en2m/README.md) |

其余还有 [concurrency.md](docs/concurrency.md)（线程与锁）、
[persistence.md](docs/persistence.md)（NVS）、
[events.md](docs/events.md)（12 个事件）、
[wiring.md](docs/wiring.md)（接线）。

---

## License

MIT

# ESP-NOW 2 MQTT — Firmware（ESP-IDF）

本仓库提供 **ESP-IDF** 组件 **`en2m`**，以及协调器 / 路由 / 终端设备示例。

分层方式对齐 **ESP-Matter**：

- **交互层**（Endpoint / Cluster / 命令 / 上报）→ 在 `en2m`
- **传输层**（ESP-NOW 树形 mesh）→ 在 `en2m`
- **驱动层**（GPIO、DHT、PWM、电表 IC…）→ **你的应用**（本仓库 `firmware/drivers` 仅作参考）

配套仓库：

- Bridge：https://github.com/SFNFIH/espnow2mqtt-bridge  
- HA 集成：https://github.com/SFNFIH/espnow2mqtt-ha  
- 总览：https://github.com/SFNFIH/espnow2mqtt  

---

## 架构

```
┌─────────────────────────────┐
│  app_main + drivers         │  ← 你写的硬件逻辑
└─────────────▲───────────────┘
              │ driver ops (get / set)
┌─────────────┴───────────────┐
│  en2m_model                 │  ← Endpoint / Cluster / Command
└─────────────▲───────────────┘
              │
┌─────────────┴───────────────┐
│  en2m_mesh                  │  ← ESP-NOW 传输 + 组网
└─────────────────────────────┘
```

协调器通过 **USB Serial/JTAG** 与主机通信（不是 Wi‑Fi STA）。全网 ESP-NOW 使用 **固定信道**（默认 channel 1，见 Kconfig / `EN2M_WIFI_CHANNEL`）。

---

## 仓库结构

| 路径 | 说明 |
|------|------|
| `components/en2m/` | 核心组件：mesh + model |
| `components/en2m_example_common/` | 示例共用辅助（如有） |
| `firmware/coordinator/` | ESP32-S3 USB 协调器 |
| `firmware/router/` | 常电转发节点（无应用 cluster） |
| `firmware/leaf/` | 最小 leaf 骨架 |
| `firmware/drivers/` | 参考驱动（继电器、门磁、DHT…） |
| `firmware/examples/` | 把驱动绑到 cluster 的完整示例 |
| `protocol/PROTOCOL.md` | 空中包 + 主机 NDJSON 约定 |
| `docs/` | 架构、快速开始、接线、角色、示例表 |

把 `components/en2m` 拷进自己的 IDF 工程，`REQUIRES en2m` 即可复用。

---

## 环境要求

- [ESP-IDF 5.x](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/)
- 协调器：**ESP32-S3**（原生 USB Serial/JTAG）
- 终端 / 路由：推荐 **ESP32-C3**（示例默认 `sdkconfig.defaults` 已设）

```bash
# 协调器
cd firmware/coordinator
idf.py set-target esp32s3
idf.py build flash monitor

# 示例设备（以继电器为例）
cd firmware/examples/relay_switch
idf.py set-target esp32c3
idf.py build flash monitor
```

烧录后：主机上应出现串口（常见 `/dev/ttyACM0` 或 `/dev/serial/by-id/...`），再启动 Bridge。

---

## Mesh 角色

初始化时选定角色（`en2m_config_t.role`）：

| 角色 | 宏 | 转发 | Beacon | 典型供电 |
|------|-----|------|--------|----------|
| Coordinator | `EN2M_ROLE_COORDINATOR` | 树根 | cost=0 | USB |
| Router | `EN2M_ROLE_ROUTER` | 是 | parent+1 | 常电 |
| Leaf | `EN2M_ROLE_LEAF` | 否 | 否 | 可电池 |

配对：主机通过 Bridge 发 `permit_join` → 协调器进入配对窗口 → 设备上电 / 复位加入。详见 [`docs/mesh-roles.md`](docs/mesh-roles.md)。

---

## 已实现 Cluster（Matter 风格子集）

| Cluster | ID | 上报 `caps` | HA 实体 |
|---------|----|-------------|---------|
| OnOff | `0x0006` | `switch`（无 Level/Color 时） | Switch |
| Level Control | `0x0008` | `light` | Light 亮度 |
| Color Control | `0x0300` | `light` | Light 色温（mireds） |
| Boolean State | `0x0045` | `contact` | Binary sensor |
| Occupancy | `0x0406` | `occupancy` / `motion` | Binary sensor |
| Illuminance | `0x0400` | `illuminance` | Sensor (lux) |
| Temperature | `0x0402` | `temperature` | Sensor |
| Relative Humidity | `0x0405` | `humidity` | Sensor |
| Pressure | `0x0403` | `pressure` | Sensor |
| Electrical Power | `0x0B04` | `power` / `energy` | Sensor |
| Fan Control | `0x0202` | `fan` | Fan |
| Window Covering | `0x0102` | `cover` | Cover |
| Door Lock | `0x0101` | `lock` | Lock |
| Thermostat | `0x0201` | `climate` | Climate |
| Smoke CO | `0x005C` | `smoke` / `carbon_monoxide` | Binary sensor |

规则摘要：

- 有 **Level 或 Color** → 上报 `light`（不再报纯 `switch`）
- 仅有 OnOff → 上报 `switch`
- 空中 JSON 上限 **`EN2M_DATA_MAX`（160 字节）**：扁平字段 + `caps` + 短 `clusters` 列表，不灌大段嵌套属性

---

## 应用怎么写（绑驱动）

```c
#include "en2m.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* 你的驱动 */
esp_err_t my_relay_set(bool on, void *ctx);
esp_err_t my_relay_get(bool *on, void *ctx);

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    en2m_endpoint_t *ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_on_off(ep, &(en2m_on_off_driver_t){
        .set = my_relay_set,
        .get = my_relay_get,
    }));

    en2m_config_t mesh = {
        .role = EN2M_ROLE_LEAF,
        .name = "relay1",   /* MQTT slug 来源之一 */
        .model = "my-sw",
    };
    ESP_ERROR_CHECK(en2m_model_start(&mesh));

    while (1) {
        en2m_model_loop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

本地状态变化时调用：

```c
en2m_model_notify(1, EN2M_CLUSTER_ON_OFF, true);  /* immediate report */
```

API 声明见 [`components/en2m/include/en2m_model.h`](components/en2m/include/en2m_model.h)。

---

## 示例一览

| 目录 | Cluster | 驱动 | 说明 |
|------|---------|------|------|
| `examples/th_sensor` | Temp + Humidity | `drv_dht` | DHT22 |
| `examples/contact_sensor` | Boolean State | `drv_gpio_contact` | 门磁 |
| `examples/relay_switch` | OnOff | `drv_gpio_relay` | HA Switch |
| `examples/smart_plug` | OnOff + Electrical | 继电器 + 功率 stub | 插座 |
| `examples/dimmable_light` | OnOff + Level + Color | 内存 stub | HA Light |
| `examples/fan_controller` | Fan Control | stub | HA Fan |
| `examples/window_cover` | Window Covering | stub | HA Cover |
| `examples/door_lock` | Door Lock | stub | HA Lock |
| `examples/thermostat` | Thermostat | stub | HA Climate |
| `examples/occupancy_sensor` | Occupancy + Illum + Smoke | stub | 传感 |

Stub 示例用于先打通 MQTT / HA 实体，再换成真实 PWM、电机、锁控等驱动。  
接线见 [`docs/wiring.md`](docs/wiring.md)、[`docs/examples.md`](docs/examples.md)。

---

## 状态上报与命令（摘要）

设备上行 state（经协调器 → Bridge → MQTT `…/<slug>/state`）示例：

```json
{
  "caps": ["light"],
  "clusters": ["on_off", "level", "color"],
  "node_role": "leaf",
  "switch": "ON",
  "brightness": 200,
  "color_temp": 300,
  "color_mode": "color_temp"
}
```

下行控制（MQTT `…/<slug>/set`，Bridge 转到 ESP-NOW CMD）：

```json
{"switch":"ON","brightness":180,"color_temp":370}
{"cover":"OPEN"}
{"lock":"UNLOCK"}
{"fan_mode":"auto","percentage":40}
{"hvac_mode":"heat","target_temperature":22}
```

也支持 Matter 风格 cluster 命令：

```json
{"ep":1,"cluster":"on_off","command":"toggle"}
{"ep":1,"cluster":"level_control","command":"move_to_level","level":128}
{"ep":1,"cluster":"window_covering","command":"go_to","position":50}
```

完整约定见 [`protocol/PROTOCOL.md`](protocol/PROTOCOL.md)。

---

## 单位约定（与 Matter 对齐处）

| 量 | 驱动侧 | 上报扁平字段 |
|----|--------|--------------|
| 温度 | 0.01 °C（`int16`） | `temperature` °C |
| 湿度 | 0.01 % | `humidity` % |
| 亮度 | 0–254 | `brightness` / `level` |
| 色温 | mireds | `color_temp` |
| 窗帘位置 | 0=开 … 100=关 | `position`（HA Cover 会再映射成 0=关/100=开） |
| 功率 | mW | `power` W |
| 电量 | mWh | `energy` Wh |

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [docs/quickstart.md](docs/quickstart.md) | 从烧录到配对 |
| [docs/architecture.md](docs/architecture.md) | 分层说明 |
| [docs/mesh-roles.md](docs/mesh-roles.md) | Coordinator / Router / Leaf |
| [docs/wiring.md](docs/wiring.md) | 引脚与信道 |
| [docs/examples.md](docs/examples.md) | 示例与驱动对照 |
| [protocol/PROTOCOL.md](protocol/PROTOCOL.md) | 包格式与 JSON |
| [components/en2m/README.md](components/en2m/README.md) | 组件简介 |

---

## License

MIT

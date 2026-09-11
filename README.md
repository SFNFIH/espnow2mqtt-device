# ESP-NOW 2 MQTT — 设备（ESP32-C3）

**本仓库 = C3（及同类）终端 / 路由设备固件。**

- ESP-IDF 组件 **`en2m`**：mesh + Matter 风格 Endpoint/Cluster（交互层）
- **`examples/*`**：开关、灯、风扇、窗帘、锁、温控、传感等示例（仓库根目录）
- **`drivers/`**：参考驱动（GPIO / DHT…），**不属于** `en2m`
- **`firmware/router`**：常电转发节点
- **`firmware/leaf`**：最小 leaf 骨架

S3 主机（协调器 + Bridge）在：**[espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host)**  
HA 插件在：**[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)**  
总览：**[espnow2mqtt](https://github.com/SFNFIH/espnow2mqtt)**

```
你的驱动 → en2m model → en2m mesh --ESP-NOW--> S3 主机仓库里的协调器
```

---

## 和 ESP-Matter 一样的分层

| 层 | 在哪 |
|----|------|
| 驱动（GPIO/I2C/DHT/PWM…） | 你的 `app_main` / `drivers/` |
| 交互（Endpoint / Cluster / 命令） | `components/en2m` → `en2m_model` |
| 传输（ESP-NOW 树） | `components/en2m` → `en2m_mesh` |

**`en2m` 不管硬件**，只提供 driver ops 回调接口。

---

## 快速烧录（C3）

需要 [ESP-IDF 5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/)。

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

| 目录 | 能力 | HA 实体 |
|------|------|---------|
| `th_sensor` | 温湿度 | Sensor |
| `contact_sensor` | 门磁 | Binary sensor |
| `relay_switch` | OnOff | Switch |
| `smart_plug` | OnOff + 功率 | Switch + Sensor |
| `dimmable_light` | 亮度 + 色温 | Light |
| `fan_controller` | 风扇 | Fan |
| `window_cover` | 窗帘 | Cover |
| `door_lock` | 门锁 | Lock |
| `thermostat` | 温控 | Climate |
| `occupancy_sensor` | 人体/光照/烟雾 | Binary + Sensor |

部分示例是 **内存 stub**，先打通 MQTT/HA，再换成真实驱动。

---

## 自建设备

```c
#include "en2m.h"

en2m_endpoint_t *ep = en2m_endpoint_create(1);
en2m_endpoint_add_on_off(ep, &my_driver);

en2m_config_t mesh = {
    .role = EN2M_ROLE_LEAF,  /* 或 EN2M_ROLE_ROUTER */
    .name = "relay1",
    .model = "my-sw",
};
en2m_model_start(&mesh);

for (;;) {
    en2m_model_loop();
}
```

已实现 Cluster（Matter ID 子集）见下方；详情 [`docs/examples.md`](docs/examples.md)、[`components/en2m/README.md`](components/en2m/README.md)。

| Cluster | HA `caps` |
|---------|-----------|
| OnOff | `switch` |
| Level / ColorControl | `light` |
| Fan / Cover / Lock / Thermostat | `fan` / `cover` / `lock` / `climate` |
| Temp / Humidity / Pressure / Illuminance / Power | 对应 sensor |
| Boolean / Occupancy / Smoke | binary_sensor |

上报 JSON ≤ `EN2M_DATA_MAX`（160），用扁平字段 + `caps`。

---

## 仓库结构

```
examples/                     # 示例工程（根目录）
drivers/                      # 参考驱动
components/en2m/              # 核心组件
firmware/router/              # 路由
firmware/leaf/                # 最小 leaf
docs/
protocol/PROTOCOL.md
```

---

## 文档

- [docs/quickstart.md](docs/quickstart.md) — 配对流程（主机在 host 仓库）
- [docs/mesh-roles.md](docs/mesh-roles.md) — Leaf / Router
- [docs/wiring.md](docs/wiring.md) — 引脚
- [docs/architecture.md](docs/architecture.md) — 分层
- [protocol/PROTOCOL.md](protocol/PROTOCOL.md) — 状态/命令 JSON

---

## License

MIT

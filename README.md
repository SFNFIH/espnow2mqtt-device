# ESP-NOW 2 MQTT — Firmware (ESP-IDF)

分层方式对齐 **ESP-Matter**：组件只管 **交互层**，驱动层由应用自己绑。

```
┌─────────────────────────────┐
│  app_main + drivers/        │  ← GPIO / DHT / 电表 IC…（你自己的）
└─────────────▲───────────────┘
              │ driver ops (get/set)
┌─────────────┴───────────────┐
│  en2m model                 │  ← Endpoint / Cluster / Command（交互层）
│  OnOff · Level · Fan · …    │
└─────────────▲───────────────┘
              │
┌─────────────┴───────────────┐
│  en2m mesh                  │  ← ESP-NOW 传输
└─────────────────────────────┘
```

## 组件 `en2m`

只提供：

- Mesh 传输（coordinator / router / leaf）
- 数据模型：endpoint + cluster + 上报/命令

**不包含** 任何外设驱动。

## 已实现 Cluster（Matter ID + HA caps）

| Cluster | ID | HA `caps` / 实体 |
|---------|----|------------------|
| OnOff | 0x0006 | `switch`（无 Level 时） |
| Level Control | 0x0008 | `light`（亮度） |
| Color Control | 0x0300 | `light`（色温 mireds） |
| Boolean State | 0x0045 | `contact` |
| Occupancy Sensing | 0x0406 | `occupancy` / `motion` |
| Illuminance | 0x0400 | `illuminance` |
| Temperature | 0x0402 | `temperature` |
| Relative Humidity | 0x0405 | `humidity` |
| Pressure | 0x0403 | `pressure` |
| Electrical Power | 0x0B04 | `power` / `energy` |
| Fan Control | 0x0202 | `fan` |
| Window Covering | 0x0102 | `cover` |
| Door Lock | 0x0101 | `lock` |
| Thermostat | 0x0201 | `climate` |
| Smoke CO Alarm | 0x005C | `smoke` / `carbon_monoxide` |

> 这是 Matter / HA MQTT **常用设备子集**，不是完整 Matter Device Type 目录。
> 上报 JSON 受 `EN2M_DATA_MAX`（160）限制，使用扁平 HA 字段 + `caps`。

## 应用怎么写

```c
#include "en2m.h"

en2m_endpoint_t *ep = en2m_endpoint_create(1);
en2m_endpoint_add_on_off(ep, &my_on_off_driver);
en2m_endpoint_add_level_control(ep, &my_level_driver); /* → HA light */
en2m_model_start(&mesh_config);
```

## 仓库结构

| 路径 | 说明 |
|------|------|
| `components/en2m` | 交互层 + 传输 |
| `firmware/drivers` | 参考驱动（不属于 en2m） |
| `firmware/examples/*` | 绑定示例（含 stub 灯/风扇/窗帘/锁/温控等） |
| `firmware/coordinator` | USB 协调器 |
| `firmware/router` | 纯转发 |

## 相关仓库

- Bridge：https://github.com/SFNFIH/espnow2mqtt-bridge
- HA：https://github.com/SFNFIH/espnow2mqtt-ha

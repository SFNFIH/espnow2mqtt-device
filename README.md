# ESP-NOW 2 MQTT — Firmware (ESP-IDF)

分层方式对齐 **ESP-Matter**：组件只管 **交互层**，驱动层由应用自己绑。

```
┌─────────────────────────────┐
│  app_main + drivers/        │  ← GPIO / DHT / 电表 IC…（你自己的）
└─────────────▲───────────────┘
              │ driver ops (get/set)
┌─────────────┴───────────────┐
│  en2m model                 │  ← Endpoint / Cluster / Command（交互层）
│  OnOff · Level · Temp · …   │
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

## 应用怎么写（类似 Matter 绑驱动）

```c
#include "en2m.h"

/* 你的驱动 */
esp_err_t my_relay_set(bool on, void *ctx);
esp_err_t my_relay_get(bool *on, void *ctx);

void app_main(void)
{
    en2m_endpoint_t *ep = en2m_endpoint_create(1);
    en2m_endpoint_add_on_off(ep, &(en2m_on_off_driver_t){
        .set = my_relay_set,
        .get = my_relay_get,
    });

    en2m_config_t mesh = { .role = EN2M_ROLE_LEAF, .name = "relay1", .model = "my-sw" };
    en2m_model_start(&mesh);
    for (;;) {
        en2m_model_loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

已实现的 Cluster（Matter 风格子集）：

| Cluster | 用途 |
|---------|------|
| OnOff | 开关 / 插座通断 |
| Level Control | 亮度/风速等（API 已有，示例可扩） |
| Boolean State | 门磁等 |
| Temperature Measurement | 温度 |
| Relative Humidity | 湿度 |
| Electrical Power | 功率 / 电量 |

## 仓库结构

| 路径 | 说明 |
|------|------|
| `components/en2m` | 交互层 + 传输（IDF 组件） |
| `firmware/drivers` | **参考驱动**（不属于 en2m） |
| `firmware/examples/*` | 把参考驱动绑到 cluster 的示例 |
| `firmware/coordinator` | USB 协调器 |
| `firmware/router` | 纯转发 |

## 相关仓库

- Bridge：https://github.com/SFNFIH/espnow2mqtt-bridge  
- HA：https://github.com/SFNFIH/espnow2mqtt-ha  

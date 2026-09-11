# ESP-NOW 2 MQTT — Firmware (ESP-IDF)

ESP-IDF 工程：用 **组件 `en2m`** 做 ESP-NOW Mesh，再烧到协调器 / 路由 / 终端设备。

> 不是另搞一套「SDK」。就是普通 ESP-IDF component，和 `esp_wifi`、`nvs_flash` 一样用。

## 仓库里有什么

| 路径 | 说明 |
|------|------|
| **`components/en2m`** | ESP-IDF **组件**（Mesh 协议与收发） |
| `components/en2m_example_common` | 示例辅助（可选） |
| `firmware/coordinator` | ESP32-S3 USB 协调器工程 |
| `firmware/router` | 常电 Router 工程 |
| `firmware/examples/*` | 温湿度 / 门磁 / 开关等示例工程 |

## 在自己的工程里用 `en2m`

任选一种（和别的 IDF 组件相同）：

**1. 拷进工程**

```text
your_project/
  components/
    en2m/          ← 复制本仓库 components/en2m
  main/
```

`main/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "main.c" REQUIRES en2m)
```

**2. EXTRA_COMPONENT_DIRS**

```cmake
set(EXTRA_COMPONENT_DIRS "/path/to/espnow2mqtt-firmware/components")
```

**3. Component Manager（`idf_component.yml`）**

在工程 `main/idf_component.yml`（或根目录）：

```yaml
dependencies:
  en2m:
    git: https://github.com/SFNFIH/espnow2mqtt-firmware.git
    path: components/en2m
    version: main
```

然后：

```bash
idf.py reconfigure
```

`menuconfig` → **ESP-NOW Mesh (en2m)** 可调信道、跳数等。

## 最小代码

```c
#include "en2m.h"

void app_main(void)
{
    en2m_config_t config = {
        .role = EN2M_ROLE_LEAF,
        .name = "bedroom",
        .model = "my-sensor",
    };
    ESP_ERROR_CHECK(en2m_mesh_init(&config));
    while (1) {
        en2m_mesh_loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

## 编译本仓库示例

```bash
. $IDF_PATH/export.sh
cd firmware/coordinator && idf.py set-target esp32s3 && idf.py build flash
cd ../examples/th_sensor && idf.py set-target esp32c3 && idf.py build flash
```

## 相关仓库

- Bridge：https://github.com/SFNFIH/espnow2mqtt-bridge  
- HA 集成：https://github.com/SFNFIH/espnow2mqtt-ha  
- 总览：https://github.com/SFNFIH/espnow2mqtt  

组件细节见 [`components/en2m/README.md`](components/en2m/README.md)。

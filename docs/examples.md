# 设备示例（驱动绑定）

示例在仓库根目录 **`examples/`**。只负责 **把驱动绑到 en2m cluster**。参考 GPIO/DHT 驱动在 **`drivers/`**；灯/风扇/窗帘等示例用 **内存 stub**，方便先打通 HA 实体再换真硬件。

| 示例 | Cluster | 说明 |
|------|---------|------|
| `examples/th_sensor` | Temperature + Humidity | `drv_dht` |
| `examples/contact_sensor` | Boolean State | `drv_gpio_contact` |
| `examples/relay_switch` | OnOff | `drv_gpio_relay` → HA switch |
| `examples/smart_plug` | OnOff + Electrical Power | 继电器 + 功率 stub |
| `examples/dimmable_light` | OnOff + Level + ColorControl | HA **light**（亮度+色温） |
| `examples/fan_controller` | Fan Control | HA **fan** |
| `examples/window_cover` | Window Covering | HA **cover** |
| `examples/door_lock` | Door Lock | HA **lock** |
| `examples/thermostat` | Thermostat | HA **climate** |
| `examples/occupancy_sensor` | Occupancy + Illuminance + Smoke | 二元/光照传感器 |

自建设备：实现对应 `*_driver_t` ops，然后 `en2m_endpoint_add_*`。

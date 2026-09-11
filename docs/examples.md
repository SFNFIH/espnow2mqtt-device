# 设备示例目录

所有示例默认角色为 **LEAF**（不转发）。需要扩展覆盖时另烧 `firmware/router`。

| 示例 | 类型 | caps | 控制 | 路径 |
|------|------|------|------|------|
| 温湿度 | 纯上报 | `temperature,humidity` | 无 | `firmware/examples/th_sensor` |
| 门磁/干簧管 | 纯上报 | `contact` | 无 | `firmware/examples/contact_sensor` |
| 继电器开关 | 上报+执行 | `switch` | `{"switch":"ON\|OFF\|TOGGLE"}` | `firmware/examples/relay_switch` |
| 智能插座风格 | 上报+执行 | `switch,power,energy` | 同上（功率为演示模拟值） | `firmware/examples/smart_plug` |

## 烧录

```bash
. $HOME/esp/esp-idf/export.sh   # 按你的 IDF 路径
cd firmware/examples/th_sensor
idf.py set-target esp32c3
idf.py build flash monitor
```

改设备名：编辑对应 `main/main.c` 里 `.name = "..."`。

## 与 HA 的对应

设备上报 JSON 带 `caps` 数组后，桥接只 Discovery 对应实体：

- 有 `temperature` → 温度传感器  
- 有 `humidity` → 湿度传感器  
- 有 `contact` → 门窗 binary_sensor  
- 有 `switch` → 可控制 Switch  
- 有 `power` / `energy` → 功率 / 电量传感器  

## 接线摘要

**th_sensor**  
DHT22 DATA→GPIO4，3V3，GND，DATA 上拉 4.7k～10k。

**contact_sensor**  
干簧管/开关：GPIO9 ↔ GND（内部上拉；断开=开/ON）。

**relay_switch / smart_plug**  
继电器 IN→GPIO5（高电平）；按键 GPIO9↔GND。

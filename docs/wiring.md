# 接线说明

## ESP32-S3 协调器

仅 USB-C 接到 HA 主机（原生 USB / Serial-JTAG）。

## 示例设备（ESP32-C3）

详见 [`examples.md`](examples.md)。默认引脚：

| 示例 | GPIO | 说明 |
|------|------|------|
| th_sensor | 4 | DHT22 DATA（需上拉） |
| contact_sensor | 9 | 干簧管到 GND |
| relay_switch / smart_plug | 5 + 9 | 继电器 IN + 按键 |

## 信道

默认 Wi-Fi channel **1**，全网一致（`EN2M_WIFI_CHANNEL`）。

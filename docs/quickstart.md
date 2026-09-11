# 快速开始（ESP-IDF）

1. 安装 [ESP-IDF 5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
2. `cd firmware/coordinator && idf.py set-target esp32s3 && idf.py build flash`
3. S3 插入 HA 主机，确认 `/dev/ttyACM0`（或 by-id）
4. （可选）烧录 `firmware/router` 到常电 C3
5. 烧录 `firmware/leaf` 到远处 C3，改 `main.c` 里的名字
6. 启动 `bridge` + Mosquitto
7. `mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60` 后复位设备

# 快速开始（C3 设备）

1. 先完成主机侧（**另一仓库**）：  
   [espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host)  
   - 烧录 `firmware/coordinator` 到 ESP32-S3  
   - 启动 Bridge，MQTT 正常  
2. 安装 [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)，执行 `permit_join`
3. 本仓库烧录示例，例如：

```bash
cd firmware/examples/relay_switch
idf.py set-target esp32c3
idf.py build flash
```

4. 复位 / 上电 C3，应出现在 HA 中

（可选）常电路由：`firmware/router`

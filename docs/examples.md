# 设备示例（驱动绑定）

示例只负责 **把参考驱动绑到 en2m cluster**，外设代码在 `firmware/drivers/`，不在 `en2m` 组件内。

| 示例 | Cluster | 参考驱动 |
|------|---------|----------|
| th_sensor | Temperature + Humidity | `drv_dht` |
| contact_sensor | Boolean State | `drv_gpio_contact` |
| relay_switch | OnOff | `drv_gpio_relay` |
| smart_plug | OnOff + Electrical Power | `drv_gpio_relay` + stub metering |

自建设备：实现自己的 `*_driver_t` ops，然后 `en2m_endpoint_add_*`。

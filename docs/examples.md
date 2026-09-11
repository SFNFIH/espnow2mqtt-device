# 设备示例（回调驱动）

示例在仓库根目录 **`examples/`**。每个示例都**没有应用循环**：组件持有任务，
应用只实现回调。参考 GPIO/按键/门磁/DHT 驱动在 **`drivers/`**；灯、风扇、窗帘、
温控等示例的硬件部分是 **内存 stub**，方便先打通 HA 实体再换真硬件。

| 示例 | Cluster | 演示的库特性 |
|------|---------|--------------|
| `examples/relay_switch` | OnOff | `attribute_write` 落硬件；按键中断用 `en2m_schedule_from_isr` 走 `attribute_write`，和远程下发同一条路径 |
| `examples/th_sensor` | Temperature + Humidity | 纯 `attribute_read`：组件在上报前采样 `drv_dht`，DHT22 的 2 秒最小间隔由 `min_report_interval_ms` 保证 |
| `examples/contact_sensor` | Boolean State | GPIO 双边沿中断 → `en2m_schedule_from_isr` → `en2m_report_boolean_state`；另配 `attribute_read` 做周期性兜底 |
| `examples/smart_plug` | OnOff + Electrical Power | 两种回调混用：继电器靠 write，功率/电量靠 read 按需采样并积分 |
| `examples/dimmable_light` | OnOff + Level + ColorControl | 一个 `attribute_write` 用 `path->cluster_id` 分发三个 cluster；带 Identify 回调 |
| `examples/fan_controller` | Fan Control | `en2m_cluster_set_write_cb`：**按 cluster** 注册回调并带自己的 `ctx`，适合一个固件驱动多个互不相关的外设 |
| `examples/window_cover` | Window Covering | `command` 回调：电机不会瞬间到位，所以命令只负责启停，行程定时器用 `en2m_report_cover_position` 持续上报真实位置 |
| `examples/door_lock` | Door Lock | 持久化属性：`en2m_start` 把上次的锁状态写回执行器 |
| `examples/thermostat` | Thermostat | `write` + `read` + `changed` 三者配合，在 `changed` 里跑本地控温闭环 |
| `examples/occupancy_sensor` | Occupancy + Illuminance | `esp_timer` 当事件源（模拟 PIR），用 `en2m_schedule` 把工作交给 en2m 任务；光照走 read 回调 |

## 自建设备

1. 选设备类型建 endpoint：`en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_...)`
   （或 `en2m_endpoint_create` + `en2m_cluster_create` 自己拼）
2. 执行器：实现 `attribute_write`，按 `path->cluster_id` / `path->attribute_id` 分发
3. 拉取型传感器：实现 `attribute_read`
4. 推送型传感器：在中断或回调里调 `en2m_report_*` / `en2m_attribute_set`
5. `en2m_start(&cfg)`，结束

不要再写 `while (1) { en2m_model_loop(); }` —— 那两个 loop 现在是空函数。

# espnow2mqtt-device 文档

这里是 **C3 终端设备侧**（`en2m` 组件 + 示例 + 参考驱动）的完整文档。

另外两个仓库各有一份同样详细的 `docs/`：

| 仓库 | 内容 | 什么时候去那边 |
|---|---|---|
| [espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host/tree/main/docs) | S3 协调器固件 + Python Bridge + HA Add-on | 查 USB NDJSON 协议、MQTT 主题、部署、协调器的 peer 表和 ACK 重传 |
| [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha/tree/main/docs) | HA 自定义集成 | 查 `caps` → HA 实体的映射、单位换算、自动化写法 |

跨三个仓库的端到端流程在本仓库的 [quickstart.md](quickstart.md)。

## 一句话说明这个库是什么

`en2m` 是一个 ESP-IDF 组件，它把一块 ESP32-C3 变成 espnow2mqtt 网络里的一个设备。
分层照着 **ESP-Matter** 抄：**组件持有数据模型、任务和时序，应用持有硬件并响应回调**。
所以一个设备固件里**没有 `while(1)`，也通常不需要自己建任务**。

```c
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
        return relay_set(value->v.b);      // 组件叫你落硬件，你落
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"},
        .attribute_write = on_write,
    };
    en2m_endpoint_create_device(1, EN2M_DEVICE_TYPE_ON_OFF_PLUG);
    ESP_ERROR_CHECK(en2m_start(&cfg));     // app_main 到此返回
}
```

## 按需求找文档

| 我想…… | 看这篇 |
|---|---|
| 先跑起来、看到 HA 里出实体 | [quickstart.md](quickstart.md) |
| 知道接线和引脚 | [wiring.md](wiring.md) |
| 照着现成示例改 | [examples.md](examples.md) |
| **从零写一个自己的设备** | [usage.md](usage.md) |
| 搞懂整体架构、为什么这么分层 | [architecture.md](architecture.md) |
| **搞懂状态怎么流转的**（启动、上报、命令、入网） | [state-flow.md](state-flow.md) |
| 搞懂 endpoint / cluster / attribute 和全部 ID | [data-model.md](data-model.md) |
| 搞懂五个回调分别什么时候被调、能干什么 | [callbacks.md](callbacks.md) |
| 知道哪个函数能在哪个上下文调、会不会死锁 | [concurrency.md](concurrency.md) |
| 控制上报频率 / 看 JSON 键和单位 | [reporting.md](reporting.md) |
| 不轮询地监听库里发生的事 | [events.md](events.md) |
| 搞懂 mesh 怎么组网、怎么选父、怎么配网 | [mesh.md](mesh.md) |
| 让开关状态断电不丢 | [persistence.md](persistence.md) |
| 查某个函数的准确语义 | [api-reference.md](api-reference.md) |
| 调内存 / 调超时 / 调信道 | [kconfig.md](kconfig.md) |
| 设备不上线 / 命令不生效 / 上报被丢 | [troubleshooting.md](troubleshooting.md) |
| 从旧的 driver-ops 版本升级 | [migration.md](migration.md) |
| 看空中协议和 USB 协议 | [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md) |
| 看英文 API 速查 | [../components/en2m/README.md](../components/en2m/README.md) |

## 建议的阅读路线

**只想做个设备（90% 的人）**

1. [quickstart.md](quickstart.md) 把示例烧进去，确认 HA 里能看到
2. [examples.md](examples.md) 找一个最像你需求的示例
3. [usage.md](usage.md) 照着五步改成你的设备
4. 卡住了查 [callbacks.md](callbacks.md) 和 [api-reference.md](api-reference.md)

**想改库 / 想搞懂内部**

1. [architecture.md](architecture.md) 分层和文件职责
2. [state-flow.md](state-flow.md) 所有状态机和时序图 ← 这篇是核心
3. [concurrency.md](concurrency.md) 线程和锁
4. [data-model.md](data-model.md) → [reporting.md](reporting.md) → [mesh.md](mesh.md) 逐层往下

## 文档一览

| 文件 | 内容 |
|---|---|
| [architecture.md](architecture.md) | 三层分层、每个源文件的职责、关键设计决策与取舍 |
| [state-flow.md](state-flow.md) | 启动时序、属性状态机、上报流水线、命令流水线、入网状态机、ACK 重传状态机、持久化生命周期 |
| [data-model.md](data-model.md) | endpoint/cluster/attribute 三级模型、值类型系统与类型强制、全部 cluster 与 attribute ID、17 种设备类型配方、容量与内存 |
| [callbacks.md](callbacks.md) | `attribute_write` / `attribute_read` / `attribute_changed` / `command` / `identify` 的完整契约、三级 fall-through、两条访问路径 |
| [concurrency.md](concurrency.md) | en2m 任务、统一队列、两把互斥锁、每个 API 的调用上下文、ISR 规则、栈用量 |
| [reporting.md](reporting.md) | 四种上报模式、周期与限流、160 字节降级策略、cluster → HA JSON 键与单位的完整映射 |
| [events.md](events.md) | 12 个事件的 id、payload、触发点与运行上下文 |
| [mesh.md](mesh.md) | 三种角色、帧格式逐字段、beacon 与选父算法、路由学习与转发、配网窗口、心跳与掉线 |
| [persistence.md](persistence.md) | 哪些属性会持久化、NVS 键与 blob 布局、刷盘时机、开机回放 |
| [api-reference.md](api-reference.md) | 全部公开函数、参数、返回值、可调用上下文 |
| [usage.md](usage.md) | 从零写设备的五步、执行器/传感器/电机/多外设四种配方、常见模式与反模式 |
| [kconfig.md](kconfig.md) | 全部 Kconfig 选项、默认值、内存代价、调参建议 |
| [examples.md](examples.md) | 11 个示例逐个详解，每个演示哪个库特性 |
| [troubleshooting.md](troubleshooting.md) | 按症状排错 |
| [migration.md](migration.md) | driver-ops → 回调式的逐项迁移表 |
| [quickstart.md](quickstart.md) | 最短路径跑通 |
| [wiring.md](wiring.md) | 接线与引脚 |

## 版本

文档对应 `EN2M_FW_VERSION = "0.4.0-idf"`，空中协议版本 `EN2M_VERSION = 2`，
基于 ESP-IDF v5.5.5 验证（11 个示例 + router + S3 协调器全部零警告编译通过）。

# mesh 角色（已并入 mesh.md）

这一篇的内容已经整合进 **[mesh.md](mesh.md)**，那里写得完整得多：
三种角色的逐项对比、帧格式逐字段、beacon 与选父算法、路由学习与转发、
配网窗口、心跳与掉线判定、以及五个超时之间的层级关系。

快速回答：

| 角色 | 供电 | 转发 | 发 beacon | 用在 |
|---|---|---|---|---|
| `EN2M_ROLE_COORDINATOR` | USB（常电） | 是 | 是 | S3 协调器，在 [espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host) 仓库 |
| `EN2M_ROLE_ROUTER` | **必须常电** | 是 | 是 | `firmware/router`，用来延伸覆盖 |
| `EN2M_ROLE_LEAF` | 常电或电池 | 否 | 否 | 绝大多数设备，`examples/` 里全是这个 |

- 详细说明 → [mesh.md](mesh.md)
- 路由器固件长什么样 → [examples.md](examples.md#firmwarerouter)
- 什么时候需要加路由器 → [mesh.md](mesh.md#10-信道与-wi-fi-共存) 和
  [troubleshooting.md](troubleshooting.md#13-距离太远--rssi-太低)

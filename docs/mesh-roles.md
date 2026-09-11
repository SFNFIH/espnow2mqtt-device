# Mesh 角色（ESP-IDF 组件 `en2m`）

在固件里初始化组件时选定角色：

```c
#include "en2m.h"

en2m_config_t config = {
    .role = EN2M_ROLE_LEAF,   // 或 ROUTER / COORDINATOR
    .name = "leaf1",
    .model = "ex-th",
};
en2m_mesh_init(&config);
```

| 角色 | 转发 | Beacon | 供电 |
|------|------|--------|------|
| Coordinator | 树根 | cost=0 | USB |
| Router | 是 | cost=parent+1 | 常电 |
| Leaf | 否 | 否 | 可电池 |

自建设备：把 `components/en2m` 放进工程并 `REQUIRES en2m` 即可，无需再包一层 SDK。

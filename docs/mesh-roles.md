# Mesh 角色（ESP-IDF）

在固件 `app_main` 里选定角色（开发期写死，运行时不再改）：

```c
// firmware/leaf/main/main.c
en2m_device_app_start(EN2M_ROLE_LEAF, "leaf1", "c3-leaf");

// firmware/router/main/main.c
en2m_device_app_start(EN2M_ROLE_ROUTER, "router1", "c3-router");

// firmware/coordinator — en2m_mesh_init({ .role = EN2M_ROLE_COORDINATOR, ... })
```

| 角色 | 转发 | Beacon | 供电 |
|------|------|--------|------|
| Coordinator | 树根 | cost=0 | USB |
| Router | 是 | cost=parent+1 | 常电 |
| Leaf | 否 | 否 | 可电池 |

自研设备：依赖 `en2m` 组件，在 `en2m_config_t.role` 填角色即可。

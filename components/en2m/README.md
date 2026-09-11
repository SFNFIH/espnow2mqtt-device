# `en2m` — ESP-IDF component

普通 ESP-IDF 组件：在 STA 模式下使用 ESP-NOW，组成以 USB 协调器为根的树状 Mesh。

- **Coordinator**：USB 棒，cost=0  
- **Router**：常电，信标 + 转发  
- **Leaf**：不转发，只挂父节点  

## 依赖本组件

```cmake
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES en2m)
```

或 Component Manager（见仓库根 README）。

## API

```c
#include "en2m.h"

en2m_config_t config = {
    .role = EN2M_ROLE_LEAF,
    .name = "node1",
    .model = "ex-th",
    .on_command = NULL,   /* 纯上报则留空；开关类填回调 */
};
ESP_ERROR_CHECK(en2m_mesh_init(&config));

for (;;) {
    en2m_mesh_loop();
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

公开头文件：

| 头文件 | 内容 |
|--------|------|
| `en2m.h` | 总览（推荐 include 这个） |
| `en2m_mesh.h` | 初始化 / 收发 / 路由 API |
| `en2m_proto.h` | 空中包与 MAC 辅助 |

## 配置

`idf.py menuconfig` → **Component config → ESP-NOW Mesh (en2m)**

## 代码风格

遵循 [ESP-IDF style guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/style-guide.html)（仓库根目录有 `.clang-format`）。

## 目录

```
components/en2m/
  include/          # 对外 API
  src/              # 实现（含 en2m_priv.h）
  CMakeLists.txt
  Kconfig
  idf_component.yml
```

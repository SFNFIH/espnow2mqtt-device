# ESP-NOW mesh component (`en2m`)

ESP-IDF component implementing an ESP-NOW tree mesh (coordinator / router / leaf).

## Style

This component follows the
[ESP-IDF style guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/style-guide.html):

- 4-space indent, no tabs
- Function `{` on its own line; `if`/`for`/`while` brace on the same line
- Public symbols prefixed with `en2m_`
- File-static variables prefixed with `s_`
- File-static functions marked `static`
- Prefer `esp_err_t` + `ESP_RETURN_ON_*` / `ESP_LOGI`
- Tunables via `Kconfig` (`menuconfig` → *ESP-NOW Mesh (en2m)*)

Root `.clang-format` matches ESP-IDF defaults.

## API

```c
#include "en2m.h"

en2m_config_t config = {
    .role = EN2M_ROLE_LEAF,
    .name = "node1",
    .model = "ex-th",
    .on_command = NULL,
};
ESP_ERROR_CHECK(en2m_mesh_init(&config));

while (1) {
    en2m_mesh_loop();
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

## Layout

```
components/en2m/
  include/     public headers
  src/         implementation (+ en2m_priv.h)
  Kconfig
  idf_component.yml
```

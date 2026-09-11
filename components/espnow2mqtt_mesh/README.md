# ESP-IDF component: espnow2mqtt_mesh

Pure C mesh stack on ESP-NOW (no Arduino).

## Role selection

Pass at init (each firmware project hard-codes one role):

```c
cfg.role = EN2M_ROLE_COORDINATOR; // or ROUTER / LEAF
en2m_mesh_init(&cfg);
```

## Build

Add to firmware `CMakeLists.txt`:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
```

## USB note

Coordinator host link uses **USB Serial/JTAG** (`driver/usb_serial_jtag.h`) — not a Wi-Fi connection.

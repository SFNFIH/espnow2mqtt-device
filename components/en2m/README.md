# `en2m` ESP-IDF component

ESP-Matter-like split:

| Layer | In this component? |
|-------|--------------------|
| Interaction (endpoints / clusters / commands) | **Yes** (`en2m_model.h`) |
| Transport (ESP-NOW mesh) | **Yes** (`en2m_mesh.h`) |
| Hardware drivers (GPIO, I2C, DHT…) | **No** — bind via driver ops |

Supported clusters (HA-oriented Matter subset): OnOff, Level, ColorControl (CT),
BooleanState, Occupancy, Illuminance, Temperature, Humidity, Pressure,
ElectricalPower, FanControl, WindowCovering, DoorLock, Thermostat, SmokeCO.

```c
#include "en2m.h"

en2m_endpoint_t *ep = en2m_endpoint_create(1);
en2m_endpoint_add_on_off(ep, &my_on_off_driver);
en2m_model_start(&mesh_config);
en2m_model_loop();
```

See repository root README for architecture and examples.

# ESP-NOW 2 MQTT — Firmware (ESP-IDF)

ESP32 mesh firmware & SDK for **ESP-NOW → Home Assistant**.

| Role | Path |
|------|------|
| USB Coordinator (ESP32-S3) | `firmware/coordinator` |
| Mesh Router (ESP32-C3, mains) | `firmware/router` |
| Device examples | `firmware/examples/*` |
| Mesh SDK | `components/espnow2mqtt_mesh` |

## Related repos

- Bridge (USB ↔ MQTT): https://github.com/SFNFIH/espnow2mqtt-bridge
- Home Assistant integration: https://github.com/SFNFIH/espnow2mqtt-ha
- Umbrella overview: https://github.com/SFNFIH/espnow2mqtt

## Build

```bash
. $IDF_PATH/export.sh
cd firmware/coordinator && idf.py set-target esp32s3 && idf.py build flash
cd ../examples/th_sensor && idf.py set-target esp32c3 && idf.py build flash
```

See `docs/` for examples, wiring, and mesh roles.

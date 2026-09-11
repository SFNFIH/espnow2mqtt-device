# ESP-NOW ↔ Serial Protocol (NDJSON) + Mesh — ESP-IDF

Coordinator talks to the host over **USB Serial/JTAG** (ESP-IDF `usb_serial_jtag`),
not over Wi-Fi. On-air protocol version **2** (mesh tree).

Hello includes `"stack":"esp-idf"`.

See `components/espnow2mqtt_mesh/include/en2m_proto.h` for `en2m_pkt_t`.

Host ↔ USB commands unchanged: `ping`, `pair`, `list`, `unpair`, `cmd`.
Uplink JSON may include `hop`, `via`, `node_role`.
